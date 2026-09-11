#pragma once
// Group commit do WAL (docs/wal.md, ADR-0012, ADR-0013, ADR-0024).
//
// Agrupa appends na janela W ou até kMaxGroupBytes (64 KiB), faz padding até o bloco,
// submete via IoBackend e avança durable_lsn em ordem estritamente FIFO (I9).

#include <cstddef>
#include <cstdint>
#include <span>

#include "base/bytes.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"
#include "core/journal.hpp"
#include "wal/io_backend.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {

class GroupCommit {
 public:
  static constexpr uint32_t kMaxBuffers = 8;

  GroupCommit() noexcept = default;
  ~GroupCommit() noexcept;

  GroupCommit(const GroupCommit&) = delete;
  GroupCommit& operator=(const GroupCommit&) = delete;
  GroupCommit(GroupCommit&&) = delete;
  GroupCommit& operator=(GroupCommit&&) = delete;

  [[nodiscard]] Status init(IoBackend& be, uint32_t block_size, uint64_t window_ns) noexcept;
  void shutdown() noexcept;

  [[nodiscard]] Result<core::Appended> append(uint16_t tmpl, ByteSpan payload, uint64_t ts_ns,
                                              uint32_t epoch, Lsn lsn) noexcept;

  [[nodiscard]] Result<uint32_t> maybe_submit(IoBackend& be, uint64_t now_ns, uint64_t file_offset,
                                              uint32_t file_idx, bool force) noexcept;

  [[nodiscard]] Status reap(IoBackend& be) noexcept;

  [[nodiscard]] Lsn durable_lsn() const noexcept { return durable_lsn_; }
  [[nodiscard]] Lsn current_last_lsn() const noexcept { return cur_last_lsn_; }
  [[nodiscard]] bool halted() const noexcept { return halted_; }
  [[nodiscard]] uint32_t inflight_count() const noexcept { return inflight_count_; }
  [[nodiscard]] uint32_t current_buffer_len() const noexcept { return cur_len_; }

  void halt() noexcept { halted_ = true; }

 private:
  struct InflightGroup {
    uint64_t token = 0;
    uint32_t buf_idx = 0;
    uint32_t len = 0;
    bool in_use = false;
    bool completed = false;
    bool ok = false;
  };

  std::byte* buffers_[kMaxBuffers]{};
  bool buf_free_[kMaxBuffers]{};

  uint32_t block_size_ = kFallbackBlock;
  uint64_t window_ns_ = 100'000;

  uint32_t cur_buf_idx_ = 0;
  uint32_t cur_len_ = 0;
  uint64_t cur_deadline_ns_ = 0;
  Lsn cur_first_lsn_{};
  Lsn cur_last_lsn_{};

  InflightGroup inflight_[kMaxInflight]{};
  uint32_t head_ = 0;
  uint32_t tail_ = 0;
  uint32_t inflight_count_ = 0;

  Lsn durable_lsn_{};
  bool halted_ = false;
  bool initialized_ = false;
};

}  // namespace rv::wal
