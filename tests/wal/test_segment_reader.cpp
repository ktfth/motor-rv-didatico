// Testes do SegmentReader do WAL.
//
// Verifica validação de cabeçalho, decodificação de registros, detecção de erros
// e a travessia de padding de blocos entre grupos de commit (ADR-0013).

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "base/crc32c.hpp"
#include "wal/segment_reader.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {
namespace {

uint32_t codifica_registro(std::byte* dst, uint64_t lsn, uint16_t tmpl, uint32_t epoch,
                           uint64_t ts_ns, const std::byte* payload, uint16_t len) {
  WalHdr h{};
  h.magic = kWalMagic;
  h.crc32c = 0;
  h.lsn = lsn;
  h.ts_ns = ts_ns;
  h.epoch = epoch;
  h.tmpl = tmpl;
  h.len = len;

  std::memcpy(dst, &h, sizeof(h));
  if (len != 0) std::memcpy(dst + sizeof(h), payload, len);

  const uint32_t total = record_bytes(len);
  const uint32_t usados = static_cast<uint32_t>(sizeof(h)) + len;
  std::memset(dst + usados, 0, total - usados);

  uint32_t crc = rv::crc32c(0, dst, sizeof(h));
  crc = rv::crc32c(crc, dst + sizeof(h), len);
  std::memcpy(dst + offsetof(WalHdr, crc32c), &crc, sizeof(crc));
  return total;
}

void grava_cabecalho_segmento(std::byte* dst, uint32_t epoch, Lsn first_lsn, uint32_t block) {
  SegmentHdr sh{};
  sh.magic = kSegmentMagic;
  sh.crc32c = 0;
  sh.format_version = kSegmentFormatVersion;
  sh.partition = 0;
  sh.epoch = epoch;
  sh.first_lsn = first_lsn.v;
  sh.segment_bytes = 64u << 20;
  sh.block_size = block;
  sh.block_source = static_cast<uint8_t>(BlockSource::Fallback);
  sh.created_ts_ns = 0;
  const uint32_t crc = rv::crc32c(0, &sh, sizeof(sh));
  sh.crc32c = crc;
  std::memcpy(dst, &sh, sizeof(sh));
}

TEST(SegmentReaderTest, LeituraValidaEContinuo) {
  constexpr size_t kSz = 64u * 1024u;
  alignas(4096) std::byte mem[kSz]{};
  grava_cabecalho_segmento(mem, 0x1234, Lsn{1}, 4096);

  uint64_t off = kSegmentHdrBytes;
  for (uint64_t i = 1; i <= 10; ++i) {
    const std::byte payload[16]{std::byte{0xAA}};
    off += codifica_registro(mem + off, i, 100, 0x1234, i * 1000, payload, 16);
  }

  SegmentReader reader;
  ASSERT_TRUE(reader.open(ByteSpan{mem, kSz}, Lsn{1}).is_ok());

  core::EventView ev{};
  uint64_t lsn_esperado = 1;
  while (true) {
    auto r = reader.next(ev);
    ASSERT_TRUE(r.is_ok());
    if (!*r) break;

    EXPECT_EQ(ev.lsn.v, lsn_esperado);
    EXPECT_EQ(ev.tmpl, 100u);
    EXPECT_EQ(ev.len, 16u);
    ++lsn_esperado;
  }
  EXPECT_EQ(lsn_esperado, 11u);
  EXPECT_EQ(reader.stop_reason(), ReplayStopReason::Clean);
}

// ADR-0013: travessia de padding de zeros entre grupos
TEST(SegmentReaderTest, TravessiaDePaddingEntreGrupos) {
  constexpr size_t kSz = 64u * 1024u;
  alignas(4096) std::byte mem[kSz]{};
  grava_cabecalho_segmento(mem, 0x5555, Lsn{1}, 4096);

  // Grupo 1: 2 registros, termina em offset 4096 + 2*48 = 4192.
  // Bloco de 4096 se estende até 8192 com zeros (padding de cauda do grupo).
  uint64_t off = kSegmentHdrBytes;
  const std::byte payload[16]{};
  off += codifica_registro(mem + off, 1, 10, 0x5555, 1000, payload, 16);
  off += codifica_registro(mem + off, 2, 10, 0x5555, 2000, payload, 16);

  // Forçamos o grupo 2 a começar alinhado a 8192
  off = pad_to_block(off, 4096);
  EXPECT_EQ(off, 8192u);

  // Grupo 2: registros 3 e 4
  off += codifica_registro(mem + off, 3, 20, 0x5555, 3000, payload, 16);
  off += codifica_registro(mem + off, 4, 20, 0x5555, 4000, payload, 16);

  SegmentReader reader;
  ASSERT_TRUE(reader.open(ByteSpan{mem, kSz}, Lsn{1}).is_ok());

  core::EventView ev{};
  uint64_t lsn_esperado = 1;
  while (true) {
    auto r = reader.next(ev);
    ASSERT_TRUE(r.is_ok());
    if (!*r) break;
    EXPECT_EQ(ev.lsn.v, lsn_esperado);
    ++lsn_esperado;
  }
  EXPECT_EQ(lsn_esperado, 5u) << "leu todos os 4 registros através do padding de zeros do bloco";
  EXPECT_EQ(reader.stop_reason(), ReplayStopReason::Clean);
}

TEST(SegmentReaderTest, DetectaCrcCorrompido) {
  constexpr size_t kSz = 64u * 1024u;
  alignas(4096) std::byte mem[kSz]{};
  grava_cabecalho_segmento(mem, 0x9999, Lsn{1}, 4096);

  uint64_t off = kSegmentHdrBytes;
  const std::byte payload[16]{};
  off += codifica_registro(mem + off, 1, 10, 0x9999, 1000, payload, 16);
  const uint64_t reg2_off = off;
  off += codifica_registro(mem + off, 2, 10, 0x9999, 2000, payload, 16);

  // Corrompe um byte do registro 2
  mem[reg2_off + sizeof(WalHdr) + 2] ^= std::byte{0xFF};

  SegmentReader reader;
  ASSERT_TRUE(reader.open(ByteSpan{mem, kSz}, Lsn{1}).is_ok());

  core::EventView ev{};
  auto r1 = reader.next(ev);
  ASSERT_TRUE(r1.is_ok() && *r1);
  EXPECT_EQ(ev.lsn.v, 1u);

  auto r2 = reader.next(ev);
  ASSERT_TRUE(r2.is_ok());
  EXPECT_FALSE(*r2) << "interrompe no registro corrompido";
  EXPECT_EQ(reader.stop_reason(), ReplayStopReason::BadCrc);
  EXPECT_EQ(reader.last_valid_lsn().v, 1u);
}

TEST(SegmentReaderTest, DetectaEpochMismatch) {
  constexpr size_t kSz = 64u * 1024u;
  alignas(4096) std::byte mem[kSz]{};
  grava_cabecalho_segmento(mem, 0xAAAA, Lsn{1}, 4096);

  uint64_t off = kSegmentHdrBytes;
  const std::byte payload[16]{};
  off += codifica_registro(mem + off, 1, 10, 0xAAAA, 1000, payload, 16);
  // Registro com epoch diferente (lixo de segmento reciclado)
  off += codifica_registro(mem + off, 2, 10, 0xBBBB, 2000, payload, 16);

  SegmentReader reader;
  ASSERT_TRUE(reader.open(ByteSpan{mem, kSz}, Lsn{1}).is_ok());

  core::EventView ev{};
  auto r1 = reader.next(ev);
  ASSERT_TRUE(r1.is_ok() && *r1);
  EXPECT_EQ(ev.lsn.v, 1u);

  auto r2 = reader.next(ev);
  ASSERT_TRUE(r2.is_ok());
  EXPECT_FALSE(*r2);
  EXPECT_EQ(reader.stop_reason(), ReplayStopReason::EpochMismatch);
}

}  // namespace
}  // namespace rv::wal
