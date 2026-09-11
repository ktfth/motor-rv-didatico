#include "wal/recovery.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/metrics.hpp"
#include "base/status.hpp"
#include "core/apply.hpp"
#include "core/partition_state.hpp"
#include "core/state_image.hpp"

namespace rv::wal {
namespace {

struct FileItem {
  uint64_t lsn = 0;
  std::string path;
};

void lista_arquivos(const char* dir, const char* prefixo, const char* sufixo,
                    std::vector<FileItem>& out) {
  out.clear();
  DIR* d = ::opendir(dir ? dir : ".");
  if (d == nullptr) return;

  const size_t pref_len = std::strlen(prefixo);
  const size_t suf_len = std::strlen(sufixo);

  struct dirent* ent = nullptr;
  while ((ent = ::readdir(d)) != nullptr) {
    const char* name = ent->d_name;
    const size_t len = std::strlen(name);
    if (len <= pref_len + suf_len) continue;
    if (std::strncmp(name, prefixo, pref_len) != 0) continue;
    if (std::strcmp(name + len - suf_len, sufixo) != 0) continue;

    const char* lsn_str = name + pref_len;
    char* end = nullptr;
    const unsigned long long val = std::strtoull(lsn_str, &end, 16);
    if (end != nullptr && *end == '.') {
      std::string caminho = (dir ? std::string(dir) : std::string(".")) + "/" + name;
      out.push_back(FileItem{static_cast<uint64_t>(val), std::move(caminho)});
    }
  }
  ::closedir(d);
}

}  // namespace

Result<RecoveryReport> recover(core::PartitionState& state, Arena& arena, Metrics& metrics,
                               const char* dir, PartitionId part, WalTail& tail_out) noexcept {
  RecoveryReport report{};
  tail_out = WalTail{};

  char snap_prefix[64]{};
  std::snprintf(snap_prefix, sizeof(snap_prefix), "state_p%u_", part.v);
  std::vector<FileItem> snaps;
  lista_arquivos(dir, snap_prefix, ".img", snaps);
  std::sort(snaps.begin(), snaps.end(), [](const FileItem& a, const FileItem& b) {
    return a.lsn > b.lsn;  // mais recente primeiro
  });

  bool snap_loaded = false;
  for (size_t i = 0; i < snaps.size(); ++i) {
    int fd = ::open(snaps[i].path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
      ::close(fd);
      continue;
    }

    const auto sz = static_cast<size_t>(st.st_size);
    void* map = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) continue;

    const Status load_st =
        core::load_state_image(state, arena, ByteSpan{static_cast<const std::byte*>(map), sz});
    ::munmap(map, sz);

    if (load_st.is_ok()) {
      snap_loaded = true;
      report.image_lsn = state.applied_lsn;
      report.used_fallback_image = (i > 0);
      break;
    }
  }

  if (!snap_loaded) {
    report.image_lsn = state.applied_lsn;
  }

  char seg_prefix[64]{};
  std::snprintf(seg_prefix, sizeof(seg_prefix), "p%u_", part.v);
  std::vector<FileItem> segs;
  lista_arquivos(dir, seg_prefix, ".wal", segs);
  std::sort(segs.begin(), segs.end(), [](const FileItem& a, const FileItem& b) {
    return a.lsn < b.lsn;  // mais antigo primeiro
  });

  core::ApplyContext ctx{nullptr, &metrics};  // sem outbox durante o replay (I10)
  Lsn expected_lsn = report.image_lsn.next();
  Lsn max_valid_lsn = report.image_lsn;

  for (const auto& seg : segs) {
    int fd = ::open(seg.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kSegmentHdrBytes)) {
      ::close(fd);
      continue;
    }

    const auto sz = static_cast<size_t>(st.st_size);
    void* map = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) continue;

    SegmentReader reader;
    const Status st_open =
        reader.open(ByteSpan{static_cast<const std::byte*>(map), sz}, expected_lsn);
    if (!st_open.is_ok()) {
      ::munmap(map, sz);
      report.stop_reason = reader.stop_reason();
      break;
    }

    core::EventView ev{};
    bool parar = false;
    while (true) {
      auto next_res = reader.next(ev);
      if (!next_res.is_ok() || !*next_res) {
        break;
      }

      if (ev.lsn.v <= report.image_lsn.v) {
        continue;
      }

      const Status app_st = core::apply(state, ev, ctx);
      if (core::classify(app_st) == core::ApplyClass::Fatal) {
        report.fatal_lsn = ev.lsn;
        report.fatal_err = app_st.code();
        report.stop_reason = reader.stop_reason();
        parar = true;
        break;
      }

      if (app_st.is_ok()) {
        report.records_applied++;
      } else {
        report.records_rejected++;
      }
      expected_lsn = ev.lsn.next();
    }

    if (reader.last_valid_lsn().v > max_valid_lsn.v) {
      max_valid_lsn = reader.last_valid_lsn();
    }

    tail_out.segment_first_lsn = reader.first_lsn().v;
    tail_out.resume_offset = reader.resume_offset();
    tail_out.epoch = reader.epoch();
    tail_out.next_lsn = max_valid_lsn.next();

    ::munmap(map, sz);

    if (parar) break;

    if (reader.stop_reason() != ReplayStopReason::Clean) {
      report.stop_reason = reader.stop_reason();
      break;
    }
  }

  report.last_valid_lsn = max_valid_lsn;
  return report;
}

}  // namespace rv::wal
