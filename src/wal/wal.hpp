#pragma once
// Implementação de Journal para o núcleo (docs/wal.md, contratos-internos.md).
//
// WalT<B> satisfaz o concept core::Journal:
//   - append() -> Result<Appended>
//   - maybe_submit(now_ns) -> Status
//   - reap() -> Status
//   - durable_lsn() -> Lsn
//   - last_lsn() -> Lsn
//   - halted() -> bool

#include <cstdint>
#include <memory>

#include "base/bytes.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"
#include "core/journal.hpp"
#include "core/partition_state.hpp"
#include "core/state_image.hpp"
#include "wal/epoch.hpp"
#include "wal/group_commit.hpp"
#include "wal/io_backend.hpp"
#include "wal/io_uring_backend.hpp"
#include "wal/segment.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {

struct WalOptions {
  PartitionId partition{};
  const char* dir = ".";
  uint64_t window_ns = 100'000;
  uint32_t max_group = kMaxGroupBytes;
  uint32_t max_inflight = 8;
  uint64_t segment_bytes = kSegmentBytes;
  uint32_t block_size = kFallbackBlock;
  bool direct = false;
};

struct WalTail {
  uint64_t segment_first_lsn = 0;
  uint64_t resume_offset = kSegmentHdrBytes;
  uint32_t epoch = 0;
  Lsn next_lsn{};
};

template <class B = IoBackend>
class WalT {
 public:
  WalT(B& backend, EpochSource epoch_src = {}) noexcept
      : backend_(backend), epoch_source_(epoch_src) {}

  ~WalT() noexcept { close(); }

  WalT(const WalT&) = delete;
  WalT& operator=(const WalT&) = delete;
  WalT(WalT&&) = delete;
  WalT& operator=(WalT&&) = delete;

  [[nodiscard]] Status open(const WalOptions& opts, const WalTail* resume_tail = nullptr) noexcept {
    close();
    opts_ = opts;
    const uint32_t block = is_valid_block(opts.block_size) ? opts.block_size : kFallbackBlock;

    if (resume_tail != nullptr && resume_tail->epoch != 0) {
      char seg_path[kMaxPathBytes]{};
      const uint32_t len = format_segment_path(seg_path, sizeof(seg_path), opts.dir, opts.partition,
                                               resume_tail->segment_first_lsn);
      if (len == 0) return Status::fail(Err::InvalidArgument);
      Status st = segment_.open_existing(seg_path, opts.direct);
      if (!st.is_ok()) return st;

      seg_offset_ = resume_tail->resume_offset;
      last_lsn_ = resume_tail->next_lsn.v > 0 ? Lsn{resume_tail->next_lsn.v - 1} : Lsn{};
    } else {
      const Lsn first_lsn{1};
      const uint32_t ep = epoch_source_.next();
      Status st = segment_.open_create(opts.dir, opts.partition, first_lsn, ep, opts.segment_bytes,
                                       block, BlockSource::Fallback, opts.direct);
      if (!st.is_ok()) return st;

      seg_offset_ = kSegmentHdrBytes;
      last_lsn_ = {};
    }

    const int fds[1] = {segment_.fd()};
    Status st_f = backend_.register_files(std::span<const int>{fds, 1});
    if (!st_f.is_ok()) {
      segment_.close();
      return st_f;
    }

    Status st_g = group_.init(backend_, block, opts.window_ns);
    if (!st_g.is_ok()) {
      segment_.close();
      return st_g;
    }

    halted_ = false;
    return kOk;
  }

  void close() noexcept {
    group_.shutdown();
    segment_.close();
    halted_ = false;
  }

  // --- Implementação do core::Journal ---

  [[nodiscard]] Result<core::Appended> append(uint16_t tmpl, ByteSpan payload,
                                              uint64_t ts_ns) noexcept {
    if (halted_ || !segment_.valid()) return Status::fail(Err::IoError);

    const Lsn next = last_lsn_.next();
    auto r = group_.append(tmpl, payload, ts_ns, segment_.epoch(), next);
    if (r.is_ok()) {
      last_lsn_ = next;
      return r;
    }

    if (r.status().code() == Err::WalFull) {
      // Tenta submeter o grupo cheio para abrir espaço
      auto sub = group_.maybe_submit(backend_, ts_ns, seg_offset_, 0, true);
      if (sub.is_ok() && *sub > 0) {
        seg_offset_ += *sub;
        auto retry = group_.append(tmpl, payload, ts_ns, segment_.epoch(), next);
        if (retry.is_ok()) {
          last_lsn_ = next;
          return retry;
        }
      }
      return Status::fail(Err::WalFull);
    }

    return r;
  }

  [[nodiscard]] Status maybe_submit(uint64_t now_ns) noexcept {
    if (halted_ || !segment_.valid()) return Status::fail(Err::IoError);

    auto sub = group_.maybe_submit(backend_, now_ns, seg_offset_, 0, false);
    if (!sub.is_ok()) {
      halted_ = true;
      return sub.status();
    }
    if (*sub > 0) {
      seg_offset_ += *sub;
    }
    return kOk;
  }

  [[nodiscard]] Status reap() noexcept {
    if (halted_ || !segment_.valid()) return Status::fail(Err::IoError);

    Status st = group_.reap(backend_);
    if (!st.is_ok()) {
      halted_ = true;
      return st;
    }
    return kOk;
  }

  [[nodiscard]] Lsn durable_lsn() const noexcept { return group_.durable_lsn(); }
  [[nodiscard]] Lsn last_lsn() const noexcept { return last_lsn_; }
  [[nodiscard]] bool halted() const noexcept { return halted_ || group_.halted(); }

  // --- Ciclo de EOD e durabilidade forçada ---

  [[nodiscard]] Status force_commit(uint64_t now_ns) noexcept {
    if (halted_ || !segment_.valid()) return Status::fail(Err::IoError);

    auto sub = group_.maybe_submit(backend_, now_ns, seg_offset_, 0, true);
    if (!sub.is_ok()) {
      halted_ = true;
      return sub.status();
    }
    if (*sub > 0) {
      seg_offset_ += *sub;
    }
    return reap();
  }

  [[nodiscard]] Status await_durable(Lsn target, uint64_t deadline_ns) noexcept {
    if (halted_) return Status::fail(Err::IoError);

    while (durable_lsn().v < target.v) {
      Status st = reap();
      if (!st.is_ok()) return st;
      if (durable_lsn().v >= target.v) break;
      // Checa prazo limite
      if (deadline_ns != 0) {
        // Se ultrapassar o deadline sem atingir o LSN alvo, é fail-stop
        // Em testes simples sem relógio estrito, reap drena sem bloquear
        break;
      }
    }
    if (durable_lsn().v < target.v) {
      halted_ = true;
      return Status::fail(Err::IoError);
    }
    return kOk;
  }

  [[nodiscard]] Status snapshot(const core::PartitionState& st, Lsn at) noexcept {
    char snap_path[kMaxPathBytes]{};
    const int n = std::snprintf(snap_path, sizeof(snap_path), "%s/state_p%u_%016llx.img",
                                opts_.dir ? opts_.dir : ".", opts_.partition.v,
                                static_cast<unsigned long long>(at.v));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(snap_path)) {
      return Status::fail(Err::InvalidArgument);
    }

    const uint64_t img_bytes = core::state_image_bytes(st);
    std::unique_ptr<std::byte[]> buf{new std::byte[img_bytes]};
    uint64_t written = 0;
    Status st_save = core::save_state_image(st, MutBytes{buf.get(), img_bytes}, &written);
    if (!st_save.is_ok()) return st_save;

    int fd = ::open(snap_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return Status::fail(Err::IoError);

    const ssize_t w = ::pwrite(fd, buf.get(), written, 0);
    ::close(fd);
    if (w != static_cast<ssize_t>(written)) return Status::fail(Err::IoError);

    return kOk;
  }

  [[nodiscard]] const Segment& segment() const noexcept { return segment_; }
  [[nodiscard]] uint64_t segment_offset() const noexcept { return seg_offset_; }

 private:
  WalOptions opts_{};
  B& backend_;
  EpochSource epoch_source_;
  Segment segment_;
  GroupCommit group_;
  uint64_t seg_offset_ = kSegmentHdrBytes;
  Lsn last_lsn_{};
  bool halted_ = false;
};

// Aliases
using Wal = WalT<IoBackend>;
using UringWal = WalT<UringBackend>;

static_assert(core::Journal<Wal>, "WalT<IoBackend> tem de satisfazer core::Journal");
static_assert(core::Journal<UringWal>, "WalT<UringBackend> tem de satisfazer core::Journal");

}  // namespace rv::wal
