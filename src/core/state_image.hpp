#pragma once
// A imagem de recuperação: *stall-and-copy* (ADR-0014), gravada SEÇÃO A SEÇÃO.
//
// ---------------------------------------------------------------------------------------------
// POR QUE NÃO É MAIS UM `memcpy` DA ARENA
//
// A primeira versão copiava a arena inteira, e a ideia era elegante: como o `init` aloca em ordem
// determinística, restaurar era rodar `init` numa arena nova e copiar os bytes por cima —
// nenhum ponteiro serializado, nenhum reajustado.
//
// Elegante e errado no que importa. A arena é dimensionada por `PartitionCapacity`, não pelos
// dados: uma revisão independente mediu **126 MiB de imagem tanto com 10 negócios quanto com 20
// mil**. `docs/wal.md` mira 20–40 ms de stall para 200 MB; com a imagem proporcional à
// CONFIGURAÇÃO, o custo do fechamento diário passa a depender de quanto alguém dimensionou, e não
// de quanto o dia teve. Dobrar a capacidade para caber num pico dobra o stall de todos os dias.
//
// Agora cada coluna vira uma seção com `count` elementos e CRC próprio, e as tabelas de índice
// não são gravadas: elas são RECONSTRUÍDAS a partir das colunas densas — `account_index` a partir
// de `account_document`, `position_index` a partir de `(account, instrument)`, e assim por diante.
// Uma tabela de hash é cache de uma relação que já está nos dados; gravá-la seria gravar duas
// vezes a mesma verdade, e com o custo do fator de carga.
//
// A exceção são os conjuntos de idempotência: eles NÃO são deriváveis de coluna nenhuma — são
// dados por si (cenário golden 13). Deles gravam-se os pares ocupados, não a tabela.
//
// O que se perde: a restauração deixa de ser um `memcpy` e passa a ser um laço por seção mais a
// reconstrução dos índices. O que se ganha: a imagem de um dia parado é quilobytes.

#include <cstdint>

#include "base/arena.hpp"
#include "base/bytes.hpp"
#include "base/pair_index.hpp"
#include "base/status.hpp"
#include "core/partition_state.hpp"

namespace rv::core {

inline constexpr uint32_t kStateImageMagic = 0x324D5352U;  // "RSM2" — v2, seções
inline constexpr uint16_t kStateImageVersion = 2;

// Uma seção por coluna. A ordem é FORMATO: mudá-la exige bump de versão.
enum class StateSection : uint8_t {
  CustodyAvailable = 0, CustodyPendingBuy, CustodyPendingSell, CustodyOverdueBuy,
  CustodyOverdueSell, CustodyBlocked, CustodyLeftovers, CustodyAvgPrice, CustodyAccount,
  CustodyInstrument, CustodyFlags,

  CashCash, CashPending, CashOverdue, CashIncome, AccountDocument, AccountFirstTrade,

  InstExternalId, InstSymbol, InstIsin, InstType, InstPriceFactor, InstLotSize,
  InstClosingPrice, InstPreviousClose, InstClosingDate,

  TradeId, TradeBrokerNote, TradeAccount, TradeInstrument, TradeQty, TradePrice, TradeCash,
  TradeSettlementDate, TradeSide, TradeState, TradeNextOfAccount,

  Exceptions, AppliedActions, AppliedIncome,

  Count
};

struct StateSectionRef {
  uint64_t offset;     // do início da imagem; múltiplo de 8
  uint32_t elem_size;
  uint32_t count;
  uint32_t crc32c;
  uint32_t reserved;
};
static_assert(sizeof(StateSectionRef) == 24);

// Um par de idempotência, como ele é gravado. Não é a tabela: são as entradas.
struct AppliedKeyRecord {
  uint64_t action_id;
  uint32_t account;
  uint32_t generation;  // 0 ou 1: sem isto, a próxima rotação esqueceria o que estava na janela
};
static_assert(sizeof(AppliedKeyRecord) == 16);

struct StateImageHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t partition_id;
  uint64_t image_bytes;
  uint64_t applied_lsn;

  uint32_t business_date;
  uint32_t prev_business_date;
  uint32_t window_dates[kSettlementSlots];
  uint32_t flags;

  uint32_t custody_count;
  uint32_t cash_count;
  uint32_t instrument_count;
  uint32_t trade_count;
  uint32_t exception_count;
  uint32_t exception_dropped;

  uint32_t actions_current;
  uint32_t actions_rotation_date;
  uint32_t income_current;
  uint32_t income_rotation_date;

  uint64_t custody_checksum;  // as mesmas âncoras de `EodMarked`, conferidas na leitura
  uint64_t cash_checksum;

  PartitionCapacity cap;
  StateSectionRef sections[static_cast<std::size_t>(StateSection::Count)];
  uint32_t header_crc32c;
  uint32_t reserved;
};

// Quanto a imagem ocupa para um dado estado. Proporcional aos DADOS.
[[nodiscard]] uint64_t state_image_bytes(const PartitionState& s) noexcept;

[[nodiscard]] Status save_state_image(const PartitionState& s, MutBytes out,
                                      uint64_t* written) noexcept;

// Reconstrói `s` a partir da imagem, usando `arena` (virgem). Reconstrói também os índices densos
// e confere os dois checksums no fim — se a imagem estiver íntegra por CRC mas o estado
// reconstruído não bater com as âncoras gravadas, algo no formato mudou sem bump de versão.
[[nodiscard]] Status load_state_image(PartitionState& s, Arena& arena, ByteSpan image) noexcept;

}  // namespace rv::core
