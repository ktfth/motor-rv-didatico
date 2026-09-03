// Formato do WAL e equivalência entre backends de I/O.
//
// Três perguntas, nesta ordem:
//   1. Os bytes em disco são os que docs/wal.md descreve? (tamanho, offsets, padding, magic)
//   2. O alinhamento de I/O direto foi MEDIDO ou ASSUMIDO — e o motor sabe dizer qual?
//   3. Os três backends produzem o MESMO arquivo para a mesma sequência de `WriteRequest`?
//
// A terceira é a que sustenta ADR-0023. Um backend de teste que gravasse bytes ligeiramente
// diferentes do de produção transformaria toda a suíte de I8..I12 numa suíte sobre um formato que
// não existe. A igualdade byte a byte é o que autoriza usar `PwriteBackend` e `FaultBackend` como
// substitutos do io_uring nos testes de invariante.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "base/crc32c.hpp"
#include "wal/block_align.hpp"
#include "wal/io_backend.hpp"
#include "wal/io_uring_backend.hpp"
#include "wal/pwrite_backend.hpp"
#include "wal/testing/fault_backend.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {
namespace {

namespace fs = std::filesystem;

// `rv::wal::testing` e o `::testing` do GoogleTest têm o mesmo nome curto; o alias
// resolve a ambiguidade de uma vez, em vez de qualificar cada uso.
using rv::wal::testing::FaultBackend;

// ------------------------------------------------------------------------------------ utilidades

class DirTemp {
 public:
  DirTemp() {
    char molde[] = "motorrv_wal_XXXXXX";
    const char* p = ::mkdtemp(molde);
    if (p != nullptr) dir_ = fs::absolute(p);
  }
  ~DirTemp() {
    std::error_code ec;
    if (!dir_.empty()) fs::remove_all(dir_, ec);
  }
  DirTemp(const DirTemp&) = delete;
  DirTemp& operator=(const DirTemp&) = delete;

  [[nodiscard]] bool ok() const { return !dir_.empty(); }
  [[nodiscard]] std::string arquivo(const char* nome) const { return (dir_ / nome).string(); }

 private:
  fs::path dir_;
};

[[nodiscard]] std::vector<unsigned char> ler_tudo(const std::string& caminho) {
  std::ifstream in(caminho, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

// O que a fase seguinte (o escritor do WAL) vai fazer por evento. Aqui é do teste porque o
// escritor ainda não existe: o que se prova neste arquivo é o FORMATO, e o formato tem de ser
// reproduzível por quem só leu docs/wal.md — que é exatamente o que este código é.
[[nodiscard]] uint32_t encode_record(std::byte* dst, uint64_t lsn, uint16_t tmpl, uint32_t epoch,
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
  std::memset(dst + usados, 0, total - usados);  // padding de 8 bytes, sempre zerado

  // CRC sobre cabeçalho (campo zerado) + payload. O padding NÃO entra: ele existe para alinhar o
  // registro seguinte, não é dado, e incluí-lo obrigaria o leitor a saber o tamanho padded antes
  // de validar o CRC — ou seja, a confiar em `len` antes de tê-lo verificado.
  uint32_t crc = rv::crc32c(0, dst, sizeof(h));
  crc = rv::crc32c(crc, dst + sizeof(h), len);
  std::memcpy(dst + offsetof(WalHdr, crc32c), &crc, sizeof(crc));
  return total;
}

struct Grupo {
  uint64_t token;   // last_lsn do grupo
  uint32_t offset;  // dentro do buffer registrado, == offset no arquivo
  uint32_t len;     // já múltiplo do bloco
};

inline constexpr uint32_t kBufBytes = 64u * 1024u;
inline constexpr uint32_t kEpoch = 0xC0FFEEu;

// Um cenário determinístico: cabeçalho de segmento + três grupos de registros. Os mesmos bytes,
// sempre, para qualquer backend — é o que torna a comparação byte a byte significativa.
struct Cenario {
  alignas(4096) std::byte buf[kBufBytes]{};
  Grupo grupos[4]{};
  uint32_t n = 0;
  uint32_t block = kFallbackBlock;

  explicit Cenario(uint32_t bloco) : block(bloco) { montar(); }

  void montar() {
    std::memset(buf, 0, sizeof(buf));

    // Bloco 0: cabeçalho de segmento.
    SegmentHdr sh{};
    sh.magic = kSegmentMagic;
    sh.format_version = kSegmentFormatVersion;
    sh.partition = 3;
    sh.epoch = kEpoch;
    sh.first_lsn = 101;
    sh.segment_bytes = kSegmentBytes;
    sh.block_size = block;
    sh.block_source = static_cast<uint8_t>(BlockSource::Fallback);
    sh.created_ts_ns = 1'700'000'000'000'000'000ull;
    uint32_t crc = rv::crc32c(0, &sh, sizeof(sh));
    sh.crc32c = crc;
    std::memcpy(buf, &sh, sizeof(sh));
    grupos[n++] = Grupo{0, 0, kSegmentHdrBytes};

    // Três grupos de registros, tamanhos diferentes de propósito: um grupo que não fecha bloco
    // (padding de cauda, ADR-0013), um que fecha exatamente, e um de registro único.
    uint32_t off = kSegmentHdrBytes;
    uint64_t lsn = 101;
    // 3 blocos com padding de cauda | 2 registros que fecham o bloco exato | 1 registro
    const uint16_t tamanhos[3][3] = {{3000, 3000, 3000}, {2016, 2016, 0}, {2048, 0, 0}};
    const uint32_t quantos[3] = {3, 2, 1};

    for (uint32_t g = 0; g < 3; ++g) {
      const uint32_t inicio = off;
      for (uint32_t r = 0; r < quantos[g]; ++r) {
        const uint16_t len = tamanhos[g][r];
        std::byte carga[4096]{};
        for (uint16_t i = 0; i < len; ++i) {
          carga[i] = static_cast<std::byte>((lsn * 31u + i) & 0xFFu);
        }
        off += encode_record(buf + off, lsn, static_cast<uint16_t>(10 + g), kEpoch,
                             1'000'000ull * lsn, carga, len);
        ++lsn;
      }
      // Padding até o bloco: o resto do bloco fica zerado (já está, pelo memset inicial).
      const uint32_t fim = static_cast<uint32_t>(pad_to_block(off, block));
      grupos[n++] = Grupo{lsn - 1, inicio, fim - inicio};
      off = fim;
    }
  }
};

// Submete todos os grupos e colhe todas as completions. Devolve `false` se alguma falhar.
[[nodiscard]] bool escrever(IoBackend& be, const Cenario& c, std::vector<Completion>& colhidas) {
  for (uint32_t i = 0; i < c.n; ++i) {
    WriteRequest req{};
    req.buf = c.buf + c.grupos[i].offset;
    req.offset = c.grupos[i].offset;
    req.token = c.grupos[i].token;
    req.len = c.grupos[i].len;
    req.buf_idx = 0;
    req.file_idx = 0;
    if (be.submit(req).is_error()) return false;
  }

  colhidas.clear();
  Completion out[8]{};
  for (int voltas = 0; voltas < 100000 && colhidas.size() < c.n; ++voltas) {
    const uint32_t k = be.reap(std::span<Completion>{out, 8});
    for (uint32_t i = 0; i < k; ++i) colhidas.push_back(out[i]);
  }
  return colhidas.size() == c.n;
}

[[nodiscard]] bool registrar(IoBackend& be, int fd, Cenario& c) {
  const int fds[1] = {fd};
  if (be.register_files(std::span<const int>{fds, 1}).is_error()) return false;
  const MutBytes bufs[1] = {MutBytes{c.buf, kBufBytes}};
  return be.register_buffers(std::span<const MutBytes>{bufs, 1}).is_ok();
}

// ------------------------------------------------------------------------------- 1. layout

TEST(WalFormato, TamanhoEAlinhamento) {
  EXPECT_EQ(sizeof(WalHdr), 32u);
  EXPECT_EQ(alignof(WalHdr), 8u);
  EXPECT_EQ(sizeof(SegmentHdr), 64u);
  EXPECT_EQ(alignof(SegmentHdr), 8u);
  EXPECT_LE(sizeof(SegmentHdr), kSegmentHdrBytes);
}

TEST(WalFormato, MagicEmHexdump) {
  // O magic tem de sair LEGÍVEL no arquivo, nesta ordem. É a primeira coisa que alguém olha num
  // log suspeito, e é por isso que ele é ASCII e não um número bonito.
  WalHdr h{};
  h.magic = kWalMagic;
  unsigned char b[sizeof(WalHdr)];
  std::memcpy(b, &h, sizeof(h));
  EXPECT_EQ(b[0], 'R');
  EXPECT_EQ(b[1], 'V');
  EXPECT_EQ(b[2], 'W');
  EXPECT_EQ(b[3], '1');

  SegmentHdr s{};
  s.magic = kSegmentMagic;
  unsigned char sb[sizeof(SegmentHdr)];
  std::memcpy(sb, &s, sizeof(s));
  EXPECT_EQ(sb[0], 'R');
  EXPECT_EQ(sb[1], 'V');
  EXPECT_EQ(sb[2], 'S');
  EXPECT_EQ(sb[3], '1');
}

TEST(WalFormato, CamposNosOffsetsDeDocsWal) {
  // Grava valores distinguíveis e relê pelos offsets do documento, sem passar pela struct. É o
  // que um leitor escrito noutra linguagem faria — e o que a recuperação faz quando desconfia.
  WalHdr h{};
  h.magic = kWalMagic;
  h.crc32c = 0x11223344u;
  h.lsn = 0x0102030405060708ull;
  h.ts_ns = 0x1122334455667788ull;
  h.epoch = 0xAABBCCDDu;
  h.tmpl = 0x5566u;
  h.len = 0x0708u;

  std::byte b[sizeof(WalHdr)];
  std::memcpy(b, &h, sizeof(h));
  EXPECT_EQ(load_le<uint32_t>(b + 0), kWalMagic);
  EXPECT_EQ(load_le<uint32_t>(b + 4), 0x11223344u);
  EXPECT_EQ(load_le<uint64_t>(b + 8), 0x0102030405060708ull);
  EXPECT_EQ(load_le<uint64_t>(b + 16), 0x1122334455667788ull);
  EXPECT_EQ(load_le<uint32_t>(b + 24), 0xAABBCCDDu);
  EXPECT_EQ(load_le<uint16_t>(b + 28), 0x5566u);
  EXPECT_EQ(load_le<uint16_t>(b + 30), 0x0708u);
}

TEST(WalFormato, SegmentHdrRegistraOrigemDoBloco) {
  SegmentHdr s{};
  s.block_size = kFallbackBlock;
  s.block_source = static_cast<uint8_t>(BlockSource::Fallback);
  std::byte b[sizeof(SegmentHdr)];
  std::memcpy(b, &s, sizeof(s));
  EXPECT_EQ(load_le<uint32_t>(b + 32), kFallbackBlock);
  EXPECT_EQ(load_le<uint8_t>(b + 36), static_cast<uint8_t>(BlockSource::Fallback));
  // Padding explícito: zerado na escrita. Um byte de lixo aqui viraria diferença de CRC entre
  // dois segmentos idênticos.
  EXPECT_EQ(load_le<uint8_t>(b + 37), uint8_t{0});
  EXPECT_EQ(load_le<uint8_t>(b + 38), uint8_t{0});
  EXPECT_EQ(load_le<uint8_t>(b + 39), uint8_t{0});
}

TEST(WalFormato, TetoDePayloadCabeNoCampo) {
  // A razão de `kMaxPayload` ser 65535 e não 65536: com 65536, `len` truncaria para 0.
  WalHdr h{};
  h.len = static_cast<uint16_t>(kMaxPayload);
  EXPECT_EQ(static_cast<uint32_t>(h.len), 65535u);
  EXPECT_EQ(static_cast<uint32_t>(h.len), kMaxPayload);
  EXPECT_EQ(record_bytes(kMaxPayload), 32u + 65536u);
}

TEST(WalFormato, PaddingEArredondamento) {
  EXPECT_EQ(pad8(0), 0u);
  EXPECT_EQ(pad8(1), 8u);
  EXPECT_EQ(pad8(7), 8u);
  EXPECT_EQ(pad8(8), 8u);
  EXPECT_EQ(pad8(9), 16u);

  EXPECT_EQ(record_bytes(0), 32u);
  EXPECT_EQ(record_bytes(1), 40u);
  EXPECT_EQ(record_bytes(8), 40u);
  EXPECT_EQ(record_bytes(9), 48u);

  for (uint32_t bloco : {512u, 1024u, 4096u}) {
    EXPECT_EQ(pad_to_block(0, bloco), 0u);
    EXPECT_EQ(pad_to_block(1, bloco), bloco);
    EXPECT_EQ(pad_to_block(bloco, bloco), bloco);
    EXPECT_EQ(pad_to_block(bloco + 1, bloco), 2ull * bloco);
    EXPECT_EQ(pad_to_block(3ull * bloco, bloco), 3ull * bloco);
  }

  EXPECT_TRUE(is_valid_block(512));
  EXPECT_TRUE(is_valid_block(4096));
  EXPECT_FALSE(is_valid_block(0));
  EXPECT_FALSE(is_valid_block(256));
  EXPECT_FALSE(is_valid_block(1536));
  EXPECT_FALSE(is_valid_block(8192));
}

TEST(WalFormato, RegistroCodificadoEValidavel) {
  alignas(8) std::byte buf[256]{};
  const std::byte carga[10] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5},
                               std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};
  const uint32_t n = encode_record(buf, 42, 7, kEpoch, 12345, carga, 10);
  EXPECT_EQ(n, record_bytes(10));
  EXPECT_EQ(n, 48u);

  const auto* h = reinterpret_cast<const WalHdr*>(buf);  // NOLINT: é o que a recuperação faz
  EXPECT_EQ(h->magic, kWalMagic);
  EXPECT_EQ(h->lsn, 42u);
  EXPECT_EQ(static_cast<uint32_t>(h->len), 10u);
  EXPECT_EQ(h->epoch, kEpoch);

  // Revalidação: zera o campo, recalcula, compara. Se o padding entrasse na conta, isto falharia.
  WalHdr copia = *h;
  const uint32_t gravado = copia.crc32c;
  copia.crc32c = 0;
  uint32_t crc = rv::crc32c(0, &copia, sizeof(copia));
  crc = rv::crc32c(crc, buf + sizeof(WalHdr), 10);
  EXPECT_EQ(crc, gravado);

  // Um bit trocado no payload tem de derrubar o CRC — é a única defesa contra escrita rasgada.
  buf[sizeof(WalHdr) + 3] ^= std::byte{0x01};
  uint32_t crc2 = rv::crc32c(0, &copia, sizeof(copia));
  crc2 = rv::crc32c(crc2, buf + sizeof(WalHdr), 10);
  EXPECT_NE(crc2, gravado);
}

// -------------------------------------------------------------------------- 2. alinhamento

TEST(BlockAlign, MedidoOuAssumidoMasNuncaSilencioso) {
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  const std::string caminho = dir.arquivo("probe.bin");
  { std::ofstream out(caminho, std::ios::binary); out.put('x'); }

  const auto r = probe_block_align_path(caminho.c_str());
  ASSERT_TRUE(r.is_ok()) << "código " << static_cast<int>(r.status().code());
  const BlockAlign a = *r;

  EXPECT_TRUE(is_valid_block(a.block)) << "bloco " << a.block;
  EXPECT_NE(a.source, BlockSource::Unknown);
  // A regra que este teste existe para fixar: se o valor foi ASSUMIDO, é o fallback declarado —
  // nunca um número qualquer que por acaso funcionou.
  if (a.source == BlockSource::Fallback) {
    EXPECT_EQ(a.block, kFallbackBlock);
    EXPECT_EQ(a.mem_align, kFallbackBlock);
  }
  RecordProperty("block", static_cast<int>(a.block));
  RecordProperty("origem", to_string(a.source));
}

TEST(BlockAlign, FdInvalidoEErro) {
  const auto r = probe_block_align(-1);
  EXPECT_FALSE(r.is_ok());
  EXPECT_EQ(r.status().code(), Err::IoError);
}

// ------------------------------------------------------------- 3. equivalência entre backends

TEST(Backends, MesmosBytesParaAMesmaSequencia) {
  DirTemp dir;
  ASSERT_TRUE(dir.ok());

  const std::string p_pwrite = dir.arquivo("pwrite.seg");
  const std::string p_fault = dir.arquivo("fault.seg");
  const std::string p_uring = dir.arquivo("uring.seg");

  auto abrir = [](const std::string& p) {
    return ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  };

  // --- pwrite
  std::vector<Completion> c_pwrite;
  {
    Cenario c{kFallbackBlock};
    const int fd = abrir(p_pwrite);
    ASSERT_GE(fd, 0);
    PwriteBackend be;
    ASSERT_TRUE(registrar(be, fd, c));
    ASSERT_TRUE(escrever(be, c, c_pwrite));
    ::close(fd);
  }
  for (const Completion& k : c_pwrite) EXPECT_TRUE(k.ok()) << "token " << k.token;

  // --- fault SEM regra armada: tem de ser indistinguível do backend que ele embrulha. Se este
  //     teste falhar, todo teste de invariante escrito sobre o FaultBackend perde o valor.
  std::vector<Completion> c_fault;
  {
    Cenario c{kFallbackBlock};
    const int fd = abrir(p_fault);
    ASSERT_GE(fd, 0);
    PwriteBackend interno;
    FaultBackend be{interno};
    ASSERT_TRUE(registrar(be, fd, c));
    ASSERT_TRUE(escrever(be, c, c_fault));
    ::close(fd);
  }
  for (const Completion& k : c_fault) EXPECT_TRUE(k.ok()) << "token " << k.token;

  const auto bytes_pwrite = ler_tudo(p_pwrite);
  const auto bytes_fault = ler_tudo(p_fault);
  ASSERT_FALSE(bytes_pwrite.empty());
  EXPECT_EQ(bytes_pwrite.size(), bytes_fault.size());
  EXPECT_EQ(bytes_pwrite, bytes_fault);

  // --- io_uring: o backend de produção. Se o ring não subir (kernel antigo, contêiner sem
  //     io_uring), o teste PULA em vez de falhar — é o motivo de ADR-0023 existir.
  Cenario c{kFallbackBlock};
  UringBackend be;
  if (be.open().is_error()) {
    GTEST_SKIP() << "io_uring indisponível nesta máquina; comparação pwrite/fault já verde";
  }
  RecordProperty("defer_taskrun", be.defer_taskrun() ? 1 : 0);
  RecordProperty("single_issuer", be.single_issuer() ? 1 : 0);

  const int fd = abrir(p_uring);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(registrar(be, fd, c));
  std::vector<Completion> c_uring;
  ASSERT_TRUE(escrever(be, c, c_uring));
  ::close(fd);
  for (const Completion& k : c_uring) EXPECT_TRUE(k.ok()) << "token " << k.token;

  const auto bytes_uring = ler_tudo(p_uring);
  EXPECT_EQ(bytes_pwrite.size(), bytes_uring.size());
  EXPECT_EQ(bytes_pwrite, bytes_uring);
}

TEST(Backends, DeferTaskrunFoiPedidoENegociado) {
  UringBackend be;
  if (be.open().is_error()) GTEST_SKIP() << "io_uring indisponível";
  // docs/ambiente.md registra que esta máquina aceita as duas flags. O teste não EXIGE — exigir
  // amarraria a suíte a uma máquina —, mas fixa a implicação: DEFER_TASKRUN sem SINGLE_ISSUER é
  // impossível, e afirmar o contrário seria bug do backend.
  EXPECT_TRUE(!be.defer_taskrun() || be.single_issuer());
  RecordProperty("defer_taskrun", be.defer_taskrun() ? 1 : 0);
}

TEST(Backends, PwriteRecusaIndiceDeBufferErrado) {
  // O pwrite funcionaria com qualquer ponteiro; o io_uring devolveria EFAULT. Por isso o backend
  // fácil recusa o que o difícil recusa — sem isso, ele não é substituto, é armadilha.
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  Cenario c{kFallbackBlock};
  const std::string p = dir.arquivo("x.seg");
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);
  PwriteBackend be;
  ASSERT_TRUE(registrar(be, fd, c));

  WriteRequest req{};
  req.buf = c.buf;
  req.len = 4096;
  req.offset = 0;
  req.token = 1;
  req.buf_idx = 1;  // só o índice 0 foi registrado
  EXPECT_EQ(be.submit(req).code(), Err::InvalidArgument);

  req.buf_idx = 0;
  req.file_idx = 7;  // só o índice 0 foi registrado
  EXPECT_EQ(be.submit(req).code(), Err::InvalidArgument);

  req.file_idx = 0;
  req.buf = c.buf + kBufBytes - 16;  // escrita ultrapassa o fim do buffer registrado
  EXPECT_EQ(be.submit(req).code(), Err::InvalidArgument);
  ::close(fd);
}

TEST(Backends, PwriteEntregaNaOrdemProgramada) {
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  Cenario c{kFallbackBlock};
  const std::string p = dir.arquivo("ordem.seg");
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend be;
  ASSERT_TRUE(registrar(be, fd, c));
  be.set_completion_order(PwriteBackend::Order::Lifo);

  std::vector<Completion> colhidas;
  ASSERT_TRUE(escrever(be, c, colhidas));
  ::close(fd);

  ASSERT_EQ(colhidas.size(), c.n);
  // LIFO: o último grupo submetido volta primeiro. É o cenário de I9 — `durable_lsn` NÃO pode
  // avançar para o token que chegou primeiro se o anterior ainda não voltou.
  for (uint32_t i = 0; i < c.n; ++i) {
    EXPECT_EQ(colhidas[i].token, c.grupos[c.n - 1 - i].token);
  }
}

// ------------------------------------------------------------------------------ 4. falhas

TEST(FaultBackend, EscritaCurtaApareceComoCurta) {
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  Cenario c{kFallbackBlock};
  const std::string p = dir.arquivo("curta.seg");
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend interno;
  FaultBackend be{interno};
  ASSERT_TRUE(registrar(be, fd, c));

  const Grupo& alvo = c.grupos[1];  // o grupo de 3 blocos
  ASSERT_GT(alvo.len, kFallbackBlock);
  be.inject_short_write(alvo.token, kFallbackBlock);  // só o primeiro bloco vai ao disco

  std::vector<Completion> colhidas;
  ASSERT_TRUE(escrever(be, c, colhidas));
  ::close(fd);

  bool visto = false;
  for (const Completion& k : colhidas) {
    if (k.token != alvo.token) continue;
    visto = true;
    EXPECT_FALSE(k.ok());
    EXPECT_TRUE(k.short_write());
    EXPECT_EQ(k.res, static_cast<int32_t>(kFallbackBlock));
    // `expected` é o que o WAL PEDIU, não o que foi escrito: é o CQE de verdade.
    EXPECT_EQ(k.expected, alvo.len);
  }
  EXPECT_TRUE(visto);

  // E o disco ficou pela metade — é o que a recuperação vai encontrar como cauda rasgada.
  const auto bytes = ler_tudo(p);
  ASSERT_GE(bytes.size(), static_cast<size_t>(alvo.offset) + alvo.len);
  bool tem_zero_no_fim = true;
  for (size_t i = alvo.offset + kFallbackBlock; i < static_cast<size_t>(alvo.offset) + alvo.len; ++i) {
    if (bytes[i] != 0) { tem_zero_no_fim = false; break; }
  }
  EXPECT_TRUE(tem_zero_no_fim);
}

TEST(FaultBackend, ErroDeCqeNaoTocaODisco) {
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  Cenario c{kFallbackBlock};
  const std::string p = dir.arquivo("erro.seg");
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend interno;
  FaultBackend be{interno};
  ASSERT_TRUE(registrar(be, fd, c));

  const Grupo& alvo = c.grupos[2];
  be.inject_error(alvo.token, EIO);

  std::vector<Completion> colhidas;
  ASSERT_TRUE(escrever(be, c, colhidas));
  ::close(fd);

  bool visto = false;
  for (const Completion& k : colhidas) {
    if (k.token != alvo.token) continue;
    visto = true;
    EXPECT_EQ(k.res, -EIO);
    EXPECT_FALSE(k.ok());
    EXPECT_FALSE(k.short_write());  // erro não é escrita curta: são fail-stops de causa diferente
  }
  EXPECT_TRUE(visto);

  const auto bytes = ler_tudo(p);
  ASSERT_GE(bytes.size(), static_cast<size_t>(alvo.offset) + alvo.len);
  for (size_t i = alvo.offset; i < static_cast<size_t>(alvo.offset) + alvo.len; ++i) {
    ASSERT_EQ(bytes[i], 0) << "offset " << i;
  }
}

TEST(FaultBackend, CompletionSeguradaSimulaQuedaNoMeioDoGrupo) {
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  Cenario c{kFallbackBlock};
  const std::string p = dir.arquivo("segura.seg");
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend interno;
  FaultBackend be{interno};
  ASSERT_TRUE(registrar(be, fd, c));

  const uint64_t preso = c.grupos[1].token;
  be.hold(preso);

  for (uint32_t i = 0; i < c.n; ++i) {
    WriteRequest req{};
    req.buf = c.buf + c.grupos[i].offset;
    req.offset = c.grupos[i].offset;
    req.token = c.grupos[i].token;
    req.len = c.grupos[i].len;
    ASSERT_TRUE(be.submit(req).is_ok());
  }

  Completion out[8]{};
  std::vector<Completion> antes;
  for (int v = 0; v < 4; ++v) {
    const uint32_t k = be.reap(std::span<Completion>{out, 8});
    for (uint32_t i = 0; i < k; ++i) antes.push_back(out[i]);
  }
  EXPECT_EQ(antes.size(), c.n - 1);
  for (const Completion& k : antes) EXPECT_NE(k.token, preso);
  EXPECT_EQ(be.held_count(), 1u);

  // Do lado do WAL, um grupo que nunca completa é indistinguível de uma queda antes do commit:
  // `durable_lsn` para nele e o outbox congela ali (I10). Liberar depois prova que o pedido
  // continuava vivo, e não perdido.
  be.release_held();
  const uint32_t k = be.reap(std::span<Completion>{out, 8});
  ASSERT_EQ(k, 1u);
  EXPECT_EQ(out[0].token, preso);
  EXPECT_TRUE(out[0].ok());
  ::close(fd);
}

TEST(FaultBackend, ReordenaCompletions) {
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  Cenario c{kFallbackBlock};
  const std::string p = dir.arquivo("reordem.seg");
  const int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend interno;
  FaultBackend be{interno};
  ASSERT_TRUE(registrar(be, fd, c));
  be.set_delivery(FaultBackend::Delivery::Lifo);

  for (uint32_t i = 0; i < c.n; ++i) {
    WriteRequest req{};
    req.buf = c.buf + c.grupos[i].offset;
    req.offset = c.grupos[i].offset;
    req.token = c.grupos[i].token;
    req.len = c.grupos[i].len;
    ASSERT_TRUE(be.submit(req).is_ok());
  }
  Completion out[8]{};
  const uint32_t k = be.reap(std::span<Completion>{out, 8});
  ASSERT_EQ(k, c.n);
  for (uint32_t i = 0; i < c.n; ++i) EXPECT_EQ(out[i].token, c.grupos[c.n - 1 - i].token);
  ::close(fd);
}

// -------------------------------------------------------------------- 5. o caminho de verdade

TEST(ODireto, EscritaAlinhadaComODirectODsync) {
  // O que docs/ambiente.md diz que funciona nesta máquina. Aqui é verificado de ponta a ponta:
  // abrir com O_DIRECT|O_DSYNC, medir o alinhamento, escrever um bloco pelo backend real.
  // Pula (não falha) onde o filesystem não aceita O_DIRECT — tmpfs, por exemplo.
  DirTemp dir;
  ASSERT_TRUE(dir.ok());
  const std::string p = dir.arquivo("direto.seg");
  int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT | O_DSYNC | O_CLOEXEC, 0644);
  if (fd < 0) GTEST_SKIP() << "O_DIRECT indisponível aqui: errno=" << errno;

  const auto r = probe_block_align(fd);
  ASSERT_TRUE(r.is_ok());
  const BlockAlign a = *r;
  RecordProperty("origem", to_string(a.source));
  ASSERT_TRUE(is_valid_block(a.block));

  Cenario c{a.block};
  PwriteBackend be;
  ASSERT_TRUE(registrar(be, fd, c));

  WriteRequest req{};
  req.buf = c.buf;
  req.offset = 0;
  req.len = kSegmentHdrBytes;  // múltiplo de qualquer bloco válido, por construção do formato
  req.token = 1;
  ASSERT_TRUE(be.submit(req).is_ok());

  Completion out[1]{};
  ASSERT_EQ(be.reap(std::span<Completion>{out, 1}), 1u);
  EXPECT_TRUE(out[0].ok()) << "res=" << out[0].res;
  ::close(fd);

  const auto bytes = ler_tudo(p);
  ASSERT_GE(bytes.size(), sizeof(SegmentHdr));
  EXPECT_EQ(bytes[0], 'R');
  EXPECT_EQ(bytes[1], 'V');
  EXPECT_EQ(bytes[2], 'S');
  EXPECT_EQ(bytes[3], '1');
}

}  // namespace
}  // namespace rv::wal
