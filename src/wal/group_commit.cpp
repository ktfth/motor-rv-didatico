#include "wal/group_commit.hpp"

#include <cstdlib>
#include <cstring>

#include "base/crc32c.hpp"

namespace rv::wal {

GroupCommit::~GroupCommit() noexcept {
  shutdown();
}

void GroupCommit::shutdown() noexcept {
  for (uint32_t i = 0; i < kMaxBuffers; ++i) {
    if (buffers_[i] != nullptr) {
      std::free(buffers_[i]);
      buffers_[i] = nullptr;
    }
    buf_free_[i] = false;
  }
  initialized_ = false;
}

Status GroupCommit::init(IoBackend& be, uint32_t block_size, uint64_t window_ns) noexcept {
  shutdown();
  block_size_ = block_size;
  window_ns_ = window_ns;
  cur_buf_idx_ = 0;
  cur_len_ = 0;
  cur_deadline_ns_ = 0;
  cur_first_lsn_ = {};
  cur_last_lsn_ = {};
  head_ = 0;
  tail_ = 0;
  inflight_count_ = 0;
  durable_lsn_ = {};
  halted_ = false;

  MutBytes mut_bufs[kMaxBuffers]{};
  for (uint32_t i = 0; i < kMaxBuffers; ++i) {
    void* ptr = nullptr;
    if (::posix_memalign(&ptr, kFallbackBlock, kMaxGroupBytes) != 0 || ptr == nullptr) {
      shutdown();
      return Status::fail(Err::ArenaExhausted);
    }
    buffers_[i] = static_cast<std::byte*>(ptr);
    std::memset(buffers_[i], 0, kMaxGroupBytes);
    buf_free_[i] = true;
    mut_bufs[i] = MutBytes{buffers_[i], kMaxGroupBytes};
  }

  const Status st = be.register_buffers(std::span<const MutBytes>{mut_bufs, kMaxBuffers});
  if (!st.is_ok()) {
    shutdown();
    return st;
  }

  initialized_ = true;
  return kOk;
}

Result<core::Appended> GroupCommit::append(uint16_t tmpl, ByteSpan payload, uint64_t ts_ns,
                                           uint32_t epoch, Lsn lsn) noexcept {
  if (halted_) return Status::fail(Err::IoError);
  if (!initialized_) return Status::fail(Err::InvalidArgument);

  if (payload.size() > kMaxPayload) {
    return Status::fail(Err::OutOfRange);
  }

  const uint32_t rec_len = record_bytes(static_cast<uint32_t>(payload.size()));
  if (rec_len > kMaxGroupBytes) {
    return Status::fail(Err::OutOfRange);
  }

  if (cur_buf_idx_ >= kMaxBuffers) {
    return Status::fail(Err::WalFull);
  }

  if (cur_len_ > 0 && cur_len_ + rec_len > kMaxGroupBytes) {
    // Grupo corrente cheio: o chamador precisa dar maybe_submit primeiro
    return Status::fail(Err::WalFull);
  }

  std::byte* dst = buffers_[cur_buf_idx_] + cur_len_;
  WalHdr h{};
  h.magic = kWalMagic;
  h.crc32c = 0;
  h.lsn = lsn.v;
  h.ts_ns = ts_ns;
  h.epoch = epoch;
  h.tmpl = tmpl;
  h.len = static_cast<uint16_t>(payload.size());

  std::memcpy(dst, &h, sizeof(h));
  if (!payload.empty()) {
    std::memcpy(dst + sizeof(h), payload.data(), payload.size());
  }

  const uint32_t used = static_cast<uint32_t>(sizeof(h)) + h.len;
  if (rec_len > used) {
    std::memset(dst + used, 0, rec_len - used);
  }

  uint32_t crc = rv::crc32c(0, dst, sizeof(h));
  crc = rv::crc32c(crc, dst + sizeof(h), h.len);
  std::memcpy(dst + offsetof(WalHdr, crc32c), &crc, sizeof(crc));

  if (cur_len_ == 0) {
    cur_first_lsn_ = lsn;
    cur_deadline_ns_ = ts_ns + window_ns_;
  }

  cur_len_ += rec_len;
  cur_last_lsn_ = lsn;

  return core::Appended{lsn, dst + sizeof(WalHdr), h.len};
}

Result<uint32_t> GroupCommit::maybe_submit(IoBackend& be, uint64_t now_ns, uint64_t file_offset,
                                           uint32_t file_idx, bool force) noexcept {
  if (halted_) return Status::fail(Err::IoError);
  if (!initialized_) return Status::fail(Err::InvalidArgument);

  if (cur_len_ == 0) {
    return uint32_t{0};
  }

  if (!force && now_ns < cur_deadline_ns_ && cur_len_ < kMaxGroupBytes) {
    return uint32_t{0};
  }

  if (inflight_count_ >= kMaxInflight) {
    return Status::fail(Err::WalFull);
  }

  const auto padded = static_cast<uint32_t>(pad_to_block(cur_len_, block_size_));
  if (padded > cur_len_) {
    std::memset(buffers_[cur_buf_idx_] + cur_len_, 0, padded - cur_len_);
  }

  WriteRequest req{};
  req.buf = buffers_[cur_buf_idx_];
  req.offset = file_offset;
  req.token = cur_last_lsn_.v;
  req.len = padded;
  req.buf_idx = cur_buf_idx_;
  req.file_idx = file_idx;

  const Status st = be.submit(req);
  if (!st.is_ok()) {
    return st;
  }

  inflight_[tail_] = InflightGroup{
      .token = cur_last_lsn_.v,
      .buf_idx = cur_buf_idx_,
      .len = padded,
      .in_use = true,
      .completed = false,
      .ok = false,
  };
  tail_ = (tail_ + 1) % kMaxInflight;
  inflight_count_++;
  buf_free_[cur_buf_idx_] = false;

  cur_buf_idx_ = kMaxBuffers;
  for (uint32_t i = 0; i < kMaxBuffers; ++i) {
    if (buf_free_[i]) {
      cur_buf_idx_ = i;
      break;
    }
  }

  cur_len_ = 0;
  cur_first_lsn_ = {};
  cur_last_lsn_ = {};
  cur_deadline_ns_ = 0;

  return padded;
}

Status GroupCommit::reap(IoBackend& be) noexcept {
  if (!initialized_) return Status::fail(Err::InvalidArgument);

  Completion out[kMaxInflight]{};
  const uint32_t n = be.reap(std::span<Completion>{out, kMaxInflight});

  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t tok = out[i].token;
    for (uint32_t j = 0; j < kMaxInflight; ++j) {
      if (inflight_[j].in_use && inflight_[j].token == tok) {
        inflight_[j].completed = true;
        inflight_[j].ok = out[i].ok();
        if (!out[i].ok()) {
          halted_ = true;
        }
        break;
      }
    }
  }

  // Avanço estritamente FIFO de durable_lsn (I9)
  while (inflight_count_ > 0 && inflight_[head_].completed) {
    if (!inflight_[head_].ok) {
      halted_ = true;
      return Status::fail(Err::IoError);
    }
    durable_lsn_ = Lsn{inflight_[head_].token};
    const uint32_t b = inflight_[head_].buf_idx;
    buf_free_[b] = true;
    if (cur_buf_idx_ >= kMaxBuffers) {
      cur_buf_idx_ = b;
    }
    inflight_[head_].in_use = false;
    head_ = (head_ + 1) % kMaxInflight;
    inflight_count_--;
  }

  if (halted_) return Status::fail(Err::IoError);
  return kOk;
}

}  // namespace rv::wal
