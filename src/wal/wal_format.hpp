#pragma once
// O formato em disco do WAL — bytes e constantes, nada mais.
//
// Este arquivo é FOLHA de propósito: ele não abre arquivo, não calcula CRC, não conhece
// `IoBackend`. Quem lê o log na recuperação e quem o escreve no commit são programas diferentes,
// escritos em fases diferentes, e a única coisa que os dois PRECISAM concordar é o desenho dos
// bytes. Pôr comportamento aqui criaria a tentação de mudar o formato para acomodar quem escreve;
// com o formato isolado, mudar um campo é uma linha que o `verificador` revisa sozinha.
//
// Little-endian, conforme docs/wal.md. `base/bytes.hpp` já falha em compilação numa máquina
// big-endian, então aqui não há conversão nenhuma: a struct é a imagem do disco.

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace rv::wal {

// ---------------------------------------------------------------------------------------- magia
// Quatro caracteres ASCII, legíveis num `hexdump -C` sem tabela de conversão. O dígito final é a
// VERSÃO do layout: mudar a posição de um campo de `WalHdr` produz 'RVW2', e um leitor antigo
// para no primeiro registro em vez de interpretar campos trocados como dados válidos. É por isso
// que `WalHdr` não tem campo de versão separado — dois lugares dizendo a mesma coisa acabam
// discordando, e o magic já é lido primeiro em toda validação.
[[nodiscard]] constexpr uint32_t fourcc(char a, char b, char c, char d) noexcept {
  return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

inline constexpr uint32_t kWalMagic = fourcc('R', 'V', 'W', '1');      // registro
inline constexpr uint32_t kSegmentMagic = fourcc('R', 'V', 'S', '1');  // cabeçalho de segmento
static_assert(kWalMagic == 0x31575652u, "'RVW1' em little-endian");
static_assert(kSegmentMagic == 0x31535652u, "'RVS1' em little-endian");

// A versão do CONTEÚDO do segmento (o que o cabeçalho de 64 bytes descreve). Anda separada do
// dígito do magic porque as duas coisas mudam por motivos diferentes: acrescentar um campo no
// espaço reservado de `SegmentHdr` não invalida um registro `RVW1` já gravado.
inline constexpr uint16_t kSegmentFormatVersion = 1;

// -------------------------------------------------------------------------------------- limites
// `len` mora num uint16. Portanto o teto REPRESENTÁVEL é 65535, e não os 65536 que a frase
// "≤ 64 KiB" de docs/wal.md sugere. A diferença de um byte não é acadêmica: um payload de
// exatamente 65536 bytes seria gravado com `len == 0` por truncamento silencioso, e o replay leria
// um registro vazio seguido de 64 KiB de "lixo" que na verdade são os dados. Alargar `len` para
// uint32 custaria 4 bytes por registro para permitir mensagens que o schema não tem; recusar o
// último byte custa nada. Ver relatório da fase: a frase do documento merece o ajuste.
inline constexpr uint32_t kMaxPayload = 64u * 1024u - 1u;  // 65535
static_assert(kMaxPayload <= UINT16_MAX, "kMaxPayload tem de caber em WalHdr::len");

// O valor assumido quando o filesystem não informa o alinhamento de I/O direto. Na máquina de
// referência (btrfs) é sempre este o caminho — ver docs/ambiente.md e `block_align.hpp`.
inline constexpr uint32_t kFallbackBlock = 4096;

// Menor e maior bloco que o formato aceita. O teto é `kSegmentHdrBytes` porque a área de dados
// começa num offset fixo: se o dispositivo exigisse blocos maiores que a região do cabeçalho, o
// primeiro registro cairia no meio de um bloco e o `O_DIRECT` recusaria a escrita. Melhor recusar
// a máquina na abertura do segmento do que descobrir isso no primeiro commit.
inline constexpr uint32_t kMinBlock = 512;

// 1 GiB por segmento, nomeado pelo primeiro LSN (docs/wal.md).
inline constexpr uint64_t kSegmentBytes = 1ull << 30;

// Registro alinhado a 8 bytes: o payload SBE seguinte precisa estar alinhado para ser lido no
// lugar, sem cópia (`view_bytes` recusa endereço desalinhado).
inline constexpr uint32_t kRecordAlign = 8;

// ----------------------------------------------------------------------------- cabeçalho de registro
// 32 bytes, exatamente o desenho de docs/wal.md. Não há campo de padding porque não há BURACO:
// os `static_assert` de offset abaixo provam que os sete campos preenchem os 32 bytes sem folga.
// Um `pad_` de zero byto seria mentira; o que o projeto exige é que o padding seja EXPLÍCITO —
// e o explícito, aqui, é a prova de que ele não existe.
//
// `crc32c` cobre o cabeçalho (com o próprio campo zerado) mais o payload. Zerar o campo antes de
// calcular é o que permite ao leitor recomputar sem desmontar a struct.
struct alignas(8) WalHdr {
  uint32_t magic;   // kWalMagic
  uint32_t crc32c;  // header (com este campo = 0) + payload
  uint64_t lsn;     // monotônico por partição (I8)
  uint64_t ts_ns;   // auditoria; o replay copia, nunca compara (D2)
  uint32_t epoch;   // aleatório por segmento: impede que lixo reciclado passe por válido
  uint16_t tmpl;    // templateId SBE
  uint16_t len;     // bytes do payload, ≤ kMaxPayload
};

static_assert(sizeof(WalHdr) == 32, "docs/wal.md fixa 32 bytes");
static_assert(alignof(WalHdr) == 8);
static_assert(std::is_trivially_copyable_v<WalHdr>);
static_assert(std::is_standard_layout_v<WalHdr>);
static_assert(offsetof(WalHdr, magic) == 0);
static_assert(offsetof(WalHdr, crc32c) == 4);
static_assert(offsetof(WalHdr, lsn) == 8);
static_assert(offsetof(WalHdr, ts_ns) == 16);
static_assert(offsetof(WalHdr, epoch) == 24);
static_assert(offsetof(WalHdr, tmpl) == 28);
static_assert(offsetof(WalHdr, len) == 30);

// ------------------------------------------------------------------------- origem do block_size
// De onde veio o número que decide o alinhamento de TODA escrita do segmento.
//
// Registrar a origem é obrigatório porque na máquina de referência `statx(STATX_DIOALIGN)` não
// responde (btrfs; docs/ambiente.md) e o motor assume 4096. Um segmento gravado sob suposição e um
// segmento gravado sob medida são bytes idênticos — a diferença só aparece quando o log é lido
// noutra máquina, ou quando alguém investiga uma escrita curta. Sem este campo, a investigação
// começa por adivinhar o que o kernel respondia seis meses atrás.
enum class BlockSource : uint8_t {
  Unknown = 0,
  StatxDioAlign = 1,  // `statx` respondeu: valor MEDIDO
  Fallback = 2,       // o filesystem não informou: valor ASSUMIDO (kFallbackBlock)
  Override = 3,       // forçado por configuração ou por teste
};

[[nodiscard]] const char* to_string(BlockSource) noexcept;

// ----------------------------------------------------------------------- cabeçalho de segmento
// Ocupa o primeiro bloco do arquivo; a área de dados começa em `kSegmentHdrBytes`. Offset fixo, e
// não "um bloco", porque o leitor precisa achar o primeiro registro ANTES de saber qual era o
// bloco daquela máquina — e o próprio `block_size` está aqui dentro.
inline constexpr uint32_t kSegmentHdrBytes = 4096;
inline constexpr uint32_t kMaxBlock = kSegmentHdrBytes;

struct alignas(8) SegmentHdr {
  uint32_t magic;           // kSegmentMagic
  uint32_t crc32c;          // os 64 bytes, com este campo = 0
  uint16_t format_version;  // kSegmentFormatVersion (CODING_RULES §11)
  uint16_t partition;       // PartitionId::v
  uint32_t epoch;           // igual ao `epoch` de todo registro do segmento
  uint64_t first_lsn;       // dá nome ao arquivo
  uint64_t segment_bytes;   // capacidade pré-alocada (kSegmentBytes)
  uint32_t block_size;      // alinhamento de offset e de tamanho de toda escrita
  uint8_t block_source;     // BlockSource — MEDIDO ou ASSUMIDO, nunca implícito
  uint8_t pad0_[3];         // buraco explícito: zerado na escrita, ignorado na leitura
  uint64_t created_ts_ns;   // auditoria; fora do replay
  uint64_t reserved0_;      // = 0
  uint64_t reserved1_;      // = 0 — campo novo entra aqui sem mover nenhum offset acima
};

static_assert(sizeof(SegmentHdr) == 64);
static_assert(alignof(SegmentHdr) == 8);
static_assert(std::is_trivially_copyable_v<SegmentHdr>);
static_assert(std::is_standard_layout_v<SegmentHdr>);
static_assert(offsetof(SegmentHdr, magic) == 0);
static_assert(offsetof(SegmentHdr, crc32c) == 4);
static_assert(offsetof(SegmentHdr, format_version) == 8);
static_assert(offsetof(SegmentHdr, partition) == 10);
static_assert(offsetof(SegmentHdr, epoch) == 12);
static_assert(offsetof(SegmentHdr, first_lsn) == 16);
static_assert(offsetof(SegmentHdr, segment_bytes) == 24);
static_assert(offsetof(SegmentHdr, block_size) == 32);
static_assert(offsetof(SegmentHdr, block_source) == 36);
static_assert(offsetof(SegmentHdr, pad0_) == 37);
static_assert(offsetof(SegmentHdr, created_ts_ns) == 40);
static_assert(offsetof(SegmentHdr, reserved0_) == 48);
static_assert(offsetof(SegmentHdr, reserved1_) == 56);
static_assert(sizeof(SegmentHdr) <= kSegmentHdrBytes);

// -------------------------------------------------------------------------------- aritmética
// Tudo `constexpr`: são as contas que o `append` faz por evento e que a recuperação repete por
// registro. As duas precisam ser a MESMA conta — daí morarem aqui e não em cada lado.

[[nodiscard]] constexpr uint32_t pad8(uint32_t n) noexcept {
  return (n + (kRecordAlign - 1)) & ~(kRecordAlign - 1);
}

// Bytes que um registro ocupa no log: cabeçalho + payload arredondado para múltiplo de 8.
[[nodiscard]] constexpr uint32_t record_bytes(uint32_t payload_len) noexcept {
  return static_cast<uint32_t>(sizeof(WalHdr)) + pad8(payload_len);
}

// Arredonda para cima ao bloco do dispositivo. `block` é potência de dois (garantido por
// `is_valid_block`), então é uma máscara e não uma divisão. Em uint64 porque o operando é um
// offset dentro do segmento, e um `uint32_t` estouraria a 4 GiB no dia em que kSegmentBytes crescer.
[[nodiscard]] constexpr uint64_t pad_to_block(uint64_t len, uint32_t block) noexcept {
  const uint64_t b = block;
  return (len + b - 1u) & ~(b - 1u);
}

[[nodiscard]] constexpr bool is_valid_block(uint32_t block) noexcept {
  return block >= kMinBlock && block <= kMaxBlock && (block & (block - 1u)) == 0u;
}

static_assert(pad8(0) == 0);
static_assert(pad8(1) == 8);
static_assert(pad8(8) == 8);
static_assert(pad8(9) == 16);
static_assert(record_bytes(0) == 32);
static_assert(record_bytes(1) == 40);
static_assert(record_bytes(kMaxPayload) == 32 + 65536);
static_assert(pad_to_block(0, 4096) == 0);
static_assert(pad_to_block(1, 4096) == 4096);
static_assert(pad_to_block(4096, 4096) == 4096);
static_assert(pad_to_block(4097, 4096) == 8192);
static_assert(pad_to_block(record_bytes(100), 512) == 512);
static_assert(is_valid_block(512) && is_valid_block(4096));
static_assert(!is_valid_block(0) && !is_valid_block(256) && !is_valid_block(8192) &&
              !is_valid_block(1536));

// Onde começa o primeiro registro. Constante, não função do bloco — ver `kSegmentHdrBytes`.
[[nodiscard]] constexpr uint64_t segment_data_offset() noexcept { return kSegmentHdrBytes; }

// Maior grupo que cabe: docs/wal.md fixa 64 KiB (16 blocos de 4 KiB), o que amortiza a escrita sem
// inflar a latência do commit. É teto de GRUPO, não de registro — um registro de kMaxPayload
// ocupa 65568 bytes e portanto vai sozinho num grupo maior; quem monta o grupo trata esse caso.
inline constexpr uint32_t kMaxGroupBytes = 64u * 1024u;

}  // namespace rv::wal
