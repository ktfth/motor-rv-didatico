#pragma once
// Recuperação e replay determinístico do WAL (docs/wal.md, contratos-internos.md).
//
// Carrega o snapshot mais recente válido (ou cai para o fallback), reproduz os eventos
// pendentes até o fim limpo do log ou primeira corrupção, sem externalizar saídas (I10).

#include <cstdint>

#include "base/arena.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"
#include "core/apply.hpp"
#include "core/partition_state.hpp"
#include "wal/segment_reader.hpp"
#include "wal/wal.hpp"

namespace rv {
class Metrics;
}

namespace rv::wal {

struct RecoveryReport {
  Lsn image_lsn{};
  bool used_fallback_image = false;
  Lsn last_valid_lsn{};
  uint64_t records_applied = 0;
  uint64_t records_rejected = 0;
  ReplayStopReason stop_reason = ReplayStopReason::Clean;
  Lsn fatal_lsn{};
  Err fatal_err = Err::Ok;
  uint64_t elapsed_ns = 0;
};

[[nodiscard]] Result<RecoveryReport> recover(core::PartitionState& state, Arena& arena,
                                             Metrics& metrics, const char* dir, PartitionId part,
                                             WalTail& tail_out) noexcept;

}  // namespace rv::wal
