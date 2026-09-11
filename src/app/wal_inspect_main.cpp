// motor-rv-wal-inspect — utilitário CLI para inspeção e diagnóstico de integridade do WAL.
//
// Lê segmentos do WAL (.wal), decodifica o SegmentHdr, valida CRCs e LSNs registro a registro,
// mapeia blocos de alinhamento e relata a fronteira exata de recuperação e razões de parada.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/bytes.hpp"
#include "base/crc32c.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"
#include "wal/segment_reader.hpp"
#include "wal/wal_format.hpp"

namespace {

void print_usage(const char* prog) {
  std::printf("Uso: %s <arquivo_segmento.wal> [--verbose]\n", prog);
  std::printf("Exemplo: %s data/wal/segment_0000000000000001.wal --verbose\n", prog);
}

const char* template_name(uint16_t tmpl) noexcept {
  switch (tmpl) {
    case 1:
      return "DayOpened";
    case 2:
      return "InstrumentDefined";
    case 3:
      return "DepositCash";
    case 4:
      return "WithdrawCash";
    case 5:
      return "DepositCustody";
    case 6:
      return "WithdrawCustody";
    case 7:
      return "TradeExecuted";
    case 8:
      return "ClosingPriceSet";
    case 9:
      return "CustodyReconciled";
    case 10:
      return "EodMarked";
    default:
      return "Custom/Unknown";
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  const char* file_path = argv[1];
  bool verbose = false;
  for (int i = 2; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--verbose" || std::string_view(argv[i]) == "-v") {
      verbose = true;
    }
  }

  int fd = ::open(file_path, O_RDONLY);
  if (fd < 0) {
    std::printf("ERRO: não foi possível abrir o arquivo '%s': %s (errno=%d)\n", file_path,
                std::strerror(errno), errno);
    return 1;
  }

  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    std::printf("ERRO: fstat falhou em '%s': %s\n", file_path, std::strerror(errno));
    ::close(fd);
    return 1;
  }

  const size_t file_size = static_cast<size_t>(st.st_size);
  if (file_size < rv::wal::kSegmentHdrBytes) {
    std::printf(
        "ERRO: arquivo muito curto para conter cabeçalho de segmento (%zu bytes < %zu bytes)\n",
        file_size, static_cast<size_t>(rv::wal::kSegmentHdrBytes));
    ::close(fd);
    return 2;
  }

  void* addr = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
  ::close(fd);

  if (addr == MAP_FAILED) {
    std::printf("ERRO: mmap falhou: %s\n", std::strerror(errno));
    return 1;
  }

  auto bytes = static_cast<const std::byte*>(addr);
  rv::ByteSpan mapped{bytes, file_size};

  // 1. Inspecionar SegmentHdr
  rv::wal::SegmentHdr sh{};
  std::memcpy(&sh, bytes, sizeof(sh));

  std::printf("==================================================================\n");
  std::printf("  motor-rv-wal-inspect: Inspeção de Segmento WAL\n");
  std::printf("  Arquivo : %s\n", file_path);
  std::printf("  Tamanho : %zu bytes (%.2f MiB)\n", file_size,
              static_cast<double>(file_size) / (1024.0 * 1024.0));
  std::printf("==================================================================\n");

  std::printf("[Cabeçalho de Segmento]\n");
  std::printf("  Magic          : 0x%08X %s\n", sh.magic,
              sh.magic == rv::wal::kSegmentMagic ? "(VÁLIDO - WALS)" : "(INVÁLIDO)");
  std::printf("  Versão Formato : %u\n", sh.format_version);
  std::printf("  Partição       : %u\n", sh.partition);
  std::printf("  Epoch          : %u\n", sh.epoch);
  std::printf("  First LSN      : %lu\n", sh.first_lsn);
  std::printf("  Segment Bytes  : %lu (capacidade planejada)\n", sh.segment_bytes);
  std::printf("  Block Size     : %u bytes\n", sh.block_size);
  std::printf("  Block Source   : %u\n", sh.block_source);

  // Validar CRC do cabeçalho
  rv::wal::SegmentHdr check_sh = sh;
  check_sh.crc32c = 0;
  uint32_t expected_crc = rv::crc32c(0, &check_sh, sizeof(check_sh));
  std::printf("  CRC32C         : 0x%08X (esperado: 0x%08X) -> %s\n\n", sh.crc32c, expected_crc,
              sh.crc32c == expected_crc ? "OK" : "CORROMPIDO");

  if (sh.magic != rv::wal::kSegmentMagic || sh.crc32c != expected_crc) {
    std::printf("FALHA: Cabeçalho do segmento inválido ou corrompido. Replay abortado.\n");
    ::munmap(addr, file_size);
    return 3;
  }

  // 2. Iterar sobre registros com SegmentReader
  rv::wal::SegmentReader reader;
  auto open_st = reader.open(mapped, rv::Lsn{sh.first_lsn});
  if (!open_st.is_ok()) {
    std::printf("FALHA: reader.open falhou: código=%d\n", static_cast<int>(open_st.code()));
    ::munmap(addr, file_size);
    return 4;
  }

  std::printf("[Varredura de Registros]\n");
  uint64_t record_count = 0;
  rv::core::EventView ev{};

  while (true) {
    auto res = reader.next(ev);
    if (!res.is_ok() || !*res) {
      break;
    }
    record_count++;
    if (verbose) {
      std::printf("  #%05lu: LSN=%lu Tipo=%u (%s) Tam=%u ts=%lu\n", record_count, ev.lsn.v, ev.tmpl,
                  template_name(ev.tmpl), ev.len, ev.ts_ns);
    }
  }

  std::printf("==================================================================\n");
  std::printf("[Diagnóstico Final de Integridade]\n");
  std::printf("  Registros Válidos : %lu\n", record_count);
  std::printf("  Primeiro LSN      : %lu\n", reader.first_lsn().v);
  std::printf("  Último LSN Válido : %lu\n", reader.last_valid_lsn().v);
  std::printf("  Resume Offset     : %lu (0x%lX)\n", reader.resume_offset(),
              reader.resume_offset());
  std::printf("  Razão de Parada   : %s\n", rv::wal::to_string(reader.stop_reason()));

  int exit_code = 0;
  if (reader.stop_reason() == rv::wal::ReplayStopReason::Clean) {
    std::printf("  Estado            : ÍNTEGRO (Fim limpo de log atingido)\n");
  } else {
    std::printf("  Estado            : ANOMALIA DETECTADA (%s)\n",
                rv::wal::to_string(reader.stop_reason()));
    exit_code = 10 + static_cast<int>(reader.stop_reason());
  }
  std::printf("==================================================================\n");

  ::munmap(addr, file_size);
  return exit_code;
}
