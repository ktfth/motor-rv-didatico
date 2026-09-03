#pragma once
// O snapshot de exposição D-1 — a ÚNICA janela do núcleo para o mundo.
//
// Por que este arquivo existe e por que ele é uma folha:
//
// ADR-0003 fixou que o Open Finance lê de um snapshot D-1 imutável, nunca do motor vivo. Isso não
// é só uma regra de dados; é uma regra de ACOPLAMENTO. A borda não linka o núcleo: ela inclui
// este cabeçalho, mapeia um arquivo e lê arrays. Se um dia o núcleo mudar por inteiro, a borda
// não recompila. E se alguém tentar incluir um header de `core/` na borda, o link quebra antes de
// qualquer revisão humana — porque `rs` não linka `rv_core`.
//
// Uma ideia estrutura o formato todo: **toda seção é um array tipado (ou um blob)**. Instrumentos
// em SoA, posições em CSR, movimentos em CSR, corpos JSON pré-serializados — quatro estruturas
// diferentes atendidas por um acessor, uma checagem (`sizeof(T) == elem_size`) e um CRC por seção.
//
// E uma regra: **só offsets, nunca ponteiros.** O arquivo é mapeado em endereços diferentes em
// cada processo e a cada troca de versão.

#include <cstddef>
#include <cstdint>

#include "base/fixed.hpp"
#include "base/ids.hpp"

namespace rv::format {

inline constexpr uint32_t kExposureMagic = 0x31535652U;  // "RVS1"
inline constexpr uint16_t kExposureVersion = 1;
inline constexpr uint32_t kExposureHeaderBytes = 4096;
inline constexpr uint32_t kSettlementSlots = 3;   // D+0, D+1, D+2 — igual ao núcleo
inline constexpr uint32_t kCurrentWindowDays = 7; // /transactions-current: D-6..D

enum class Section : uint8_t {
  // ---- instrumentos, em SoA (indexados por slot de instrumento) ----
  InstrumentSymbol = 0,
  InstrumentIsin,
  InstrumentType,
  InstrumentPriceFactor,
  InstrumentLotSize,
  InstrumentClosingPrice,

  // ---- contas: endereçamento aberto sobre o hash do documento ----
  AccountSlot,

  // ---- posições em CSR por conta ----
  PositionRow,          // uint32 [contas+1]: início da faixa de posições de cada conta
  PositionInstrument,   // uint32 [posições]
  PositionAvailable,    // int64
  PositionBlocked,      // int64
  PositionLeftovers,    // int64
  PositionPendingBuy,   // int64 [posições * kSettlementSlots]
  PositionPendingSell,  // int64 [posições * kSettlementSlots]
  PositionOverdueSell,  // int64 [posições] — a falha de entrega é visível na exposição
  PositionAvgPrice,     // int64
  PositionGrossAmount,  // int64 — I7, JÁ CALCULADO (ver abaixo)
  PositionInvestmentId, // InvestmentId [posições]
  InvestmentSlot,       // endereçamento aberto: investmentId -> slot de posição (R15)

  // ---- movimentos em CSR por conta ----
  MovementRow,          // uint32 [contas+1]
  MovementRecord,       // MovementRecord [movimentos]

  // ---- notas de corretagem ----
  BrokerNoteSlot,       // endereçamento aberto: brokerNoteId -> faixa de movimentos
  BrokerNoteRange,      // BrokerNoteRange [notas]

  // ---- corpos JSON pré-serializados ----
  BodyInvestmentRef,    // BlobRef [posições]  — GET /investments/{id}
  BodyBalancesRef,      // BlobRef [posições]  — GET /investments/{id}/balances
  BodyListRef,          // BlobRef [contas]    — GET /investments
  BodyBytes,            // blob

  Count
};

struct SectionRef {
  uint64_t offset;     // do início do arquivo; múltiplo de 64
  uint64_t len;
  uint32_t elem_size;  // 0 = blob
  uint32_t count;
  uint32_t crc32c;
  uint32_t reserved;
};
static_assert(sizeof(SectionRef) == 32);

struct AccountSlotEntry {
  uint64_t document;  // 0 = vazio
  uint32_t index;     // índice da conta nas seções CSR
  uint32_t reserved;
};
static_assert(sizeof(AccountSlotEntry) == 16);

struct InvestmentSlotEntry {
  InvestmentId id;
  uint32_t position;  // 0xFFFFFFFF = vazio
  uint32_t reserved;
};
static_assert(sizeof(InvestmentSlotEntry) == 24);

// Um movimento como a API RV o expõe. 64 bytes: uma linha de cache por movimento, e o percurso
// de `/transactions-current` de uma conta é sequencial em memória.
struct MovementRecord {
  uint64_t movement_id;
  uint64_t broker_note_id;
  int64_t qty_raw;
  int64_t price_raw;
  int64_t amount_raw;
  uint32_t instrument;
  uint32_t date;      // AAAAMMDD
  uint8_t type;       // compra, venda, provento, evento corporativo
  uint8_t side;
  uint16_t flags;
  uint32_t reserved[3];
};
static_assert(sizeof(MovementRecord) == 64);

struct BrokerNoteRange {
  uint64_t broker_note_id;  // 0 = vazio
  uint32_t first_movement;
  uint32_t movement_count;
  uint32_t account;
  uint32_t reserved;
};
static_assert(sizeof(BrokerNoteRange) == 24);

struct BlobRef {
  uint64_t offset;  // dentro da seção BodyBytes
  uint32_t len;
  uint32_t reserved;
};
static_assert(sizeof(BlobRef) == 16);

struct ExposureHeader {
  uint32_t magic;
  uint16_t format_version;
  uint16_t partition_id;
  uint64_t snapshot_lsn;      // o LSN EXATO em que o loop foi pausado
  uint32_t base_date;         // D-1: a data que a API publica (R14)
  uint32_t engine_build_id;
  uint64_t created_ts_ns;     // auditoria; nada depende disto
  uint64_t file_bytes;
  uint32_t block_size;
  uint8_t block_size_origin;  // 0 = statx, 1 = fallback 4096
  uint8_t flags;              // bit0 = houve divergência de reconciliação (cenário golden 14)
  uint16_t section_count;
  uint32_t account_slot_mask;     // capacidade − 1 (potência de dois)
  uint32_t investment_slot_mask;
  uint32_t broker_note_slot_mask;
  uint32_t account_count;
  uint32_t position_count;
  uint32_t instrument_count;
  uint32_t movement_count;
  uint32_t padding_explicito;  // sem ele o compilador inseriria o mesmo padding em silêncio,
                               // e o tamanho do cabeçalho passaria a depender do compilador
  uint64_t custody_checksum;  // idênticos aos de EodMarked: a âncora atravessa o snapshot
  uint64_t cash_checksum;
  SectionRef sections[static_cast<std::size_t>(Section::Count)];
  // O espaço que sobra até 4 KiB. Ele NÃO é folga decorativa: é onde um campo novo cabe sem
  // mudar o tamanho do cabeçalho — o que significa sem invalidar o alinhamento de tudo que vem
  // depois. Bump de versão continua obrigatório; o que se evita é o bump de LAYOUT.
  static constexpr std::size_t kPrefixBytes = 96;
  uint8_t reserved[kExposureHeaderBytes - kPrefixBytes -
                   sizeof(SectionRef) * static_cast<std::size_t>(Section::Count) - 4];
  uint32_t header_crc32c;  // os últimos quatro bytes do cabeçalho
};
static_assert(sizeof(ExposureHeader) == kExposureHeaderBytes,
              "o cabeçalho é um bloco de 4 KiB exato: é o que permite lê-lo com uma leitura só");

inline constexpr uint32_t kNoSlot = 0xFFFFFFFFU;

// A função de mistura É FORMATO: escritor e leitor precisam concordar, então ela mora aqui,
// congelada, e não em cada lado. Mudá-la invalida todo snapshot existente.
[[nodiscard]] constexpr uint64_t exposure_hash(uint64_t x) noexcept { return mix64(x); }

// ---------------------------------------------------------------------------------------------
// POR QUE `PositionGrossAmount` JÁ VEM CALCULADO
//
// `grossAmount = qty × closingPrice / priceFactor` (I7) é calculado UMA vez, quando o snapshot é
// construído, e gravado. A borda não faz aritmética de dinheiro — ela recorta bytes. Duas
// consequências: I7 vira um teste de golden do construtor (e não do servidor), e não existem dois
// lugares onde a mesma conta possa divergir. O custo por requisição também cai a zero, o que é o
// ponto do desenho inteiro (ADR-0003).
//
// POR QUE OS CORPOS JSON JÁ VÊM SERIALIZADOS
//
// Mesma razão, levada ao fim: o caminho quente da borda é `lookup → fatia de bytes → resposta`.
// Serializar por requisição gastaria CPU para produzir, todo dia, o mesmo texto para o mesmo
// investidor. E é isso que permite ao servidor não ter alocação no caminho da requisição.
// ---------------------------------------------------------------------------------------------

}  // namespace rv::format
