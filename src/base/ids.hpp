#pragma once
// Quem é quem no motor. Uma regra só: todo identificador é um struct com um campo `v`, comparação
// padrão e nenhuma conversão implícita para inteiro. O custo é zero; o ganho é que passar uma
// conta onde se espera um instrumento não compila.

#include <array>
#include <compare>
#include <cstdint>
#include <type_traits>

namespace rv {

// O documento do titular: CPF (11 dígitos) ou CNPJ (14). NÃO é hash — um ledger não pode conviver
// com colisão. É a chave estável entre partições, entre dias e entre o log e o snapshot: o log
// fala `DocumentId`, a memória fala `AccountId`, e por isso rebalancear partição não invalida
// nada que esteja gravado.
struct DocumentId {
  uint64_t v = 0;
  auto operator<=>(const DocumentId&) const = default;
  bool operator==(const DocumentId&) const = default;
};

inline constexpr uint64_t kCnpjBit = 1ULL << 63;
[[nodiscard]] constexpr DocumentId cpf(uint64_t digitos) noexcept { return DocumentId{digitos}; }
[[nodiscard]] constexpr DocumentId cnpj(uint64_t digitos) noexcept {
  return DocumentId{digitos | kCnpjBit};
}
[[nodiscard]] constexpr bool is_cnpj(DocumentId d) noexcept { return (d.v & kCnpjBit) != 0; }
[[nodiscard]] constexpr uint64_t digits_of(DocumentId d) noexcept { return d.v & ~kCnpjBit; }

// Índice denso, atribuído por ordem de primeira aparição NO LOG — logo, função do prefixo, logo
// determinístico no replay (I12). Nunca aparece no log nem no snapshot como chave.
struct InstrumentId {
  uint32_t v = 0;
  auto operator<=>(const InstrumentId&) const = default;
  bool operator==(const InstrumentId&) const = default;
};

struct AccountId {
  uint32_t v = 0;
  auto operator<=>(const AccountId&) const = default;
  bool operator==(const AccountId&) const = default;
};

// `uint16` porque a máquina de referência comporta quatro partições e o formato não deve ser
// reaberto quando forem sessenta e quatro.
struct PartitionId {
  uint16_t v = 0;
  auto operator<=>(const PartitionId&) const = default;
  bool operator==(const PartitionId&) const = default;
};

// LSN é POR PARTIÇÃO. Um LSN global exigiria coordenação entre cores — o oposto de
// shared-nothing (ADR-0005). Comparar LSNs de partições diferentes não significa nada; o
// manifesto casa as partições por `EodMarked{D}`, não por LSN.
struct Lsn {
  uint64_t v = 0;
  auto operator<=>(const Lsn&) const = default;
  bool operator==(const Lsn&) const = default;
  [[nodiscard]] constexpr Lsn next() const noexcept { return Lsn{v + 1}; }
};

inline constexpr InstrumentId kNoInstrument{0};
inline constexpr AccountId kNoAccount{0};
inline constexpr Lsn kNoLsn{0};

// O `resourceId` da API Recursos e o `investmentId` da API Renda Variável são o mesmo valor
// (R15). Derivado deterministicamente de (documento, símbolo), portanto estável entre dias,
// replays e partições — nada de contador nem de UUID aleatório.
struct InvestmentId {
  std::array<uint8_t, 16> b{};
  auto operator<=>(const InvestmentId&) const = default;
  bool operator==(const InvestmentId&) const = default;
};

// Data no formato AAAAMMDD: legível em hexdump, ordena como inteiro, sem fuso e sem `std::chrono`
// no caminho quente. Para indexar bucket por data usa-se `day_index()`, que é aritmética pura.
//
// O que esta classe NÃO faz: dizer se um dia é útil. Isso é dado de calendário
// (`data/calendario-b3-2026.csv`), resolvido pelo ingress e gravado no evento. Calcular dia útil
// dentro do motor exigiria ler arquivo durante o replay e violaria I12.
struct DateYmd {
  uint32_t v = 0;  // 0 = ausente

  static constexpr DateYmd from_ymd(int y, int m, int d) noexcept {
    return DateYmd{static_cast<uint32_t>(y * 10000 + m * 100 + d)};
  }
  [[nodiscard]] constexpr int year() const noexcept { return static_cast<int>(v / 10000); }
  [[nodiscard]] constexpr int month() const noexcept { return static_cast<int>((v / 100) % 100); }
  [[nodiscard]] constexpr int day() const noexcept { return static_cast<int>(v % 100); }
  [[nodiscard]] constexpr bool empty() const noexcept { return v == 0; }

  // Dias desde 1970-01-01 no calendário gregoriano proléptico (algoritmo de Howard Hinnant).
  // Puro: sem tabela, sem laço, sem relógio.
  [[nodiscard]] constexpr int32_t day_index() const noexcept {
    int y = year();
    const int m = month();
    const int d = day();
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);              // [0, 399]
    const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
  }

  static constexpr DateYmd from_day_index(int32_t z) noexcept {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return from_ymd(y + static_cast<int>(m <= 2), static_cast<int>(m), static_cast<int>(d));
  }

  auto operator<=>(const DateYmd&) const = default;
  bool operator==(const DateYmd&) const = default;
};

static_assert(sizeof(DateYmd) == 4);
static_assert(std::is_trivially_copyable_v<DateYmd>);
static_assert(DateYmd::from_ymd(1970, 1, 1).day_index() == 0);
static_assert(DateYmd::from_ymd(2026, 9, 2).day_index() == 20698);
static_assert(DateYmd::from_day_index(20698) == DateYmd::from_ymd(2026, 9, 2));
static_assert(DateYmd::from_ymd(2026, 9, 4).day_index() - DateYmd::from_ymd(2026, 9, 2).day_index() == 2);

// A função de mistura que decide a partição. É FORMATO, não detalhe: mudar `mix64` muda para qual
// core vai cada investidor, o que reordena o log e invalida todo snapshot existente. Por isso ela
// mora aqui, congelada, e tem teste golden com valores fixos.
[[nodiscard]] constexpr uint64_t mix64(uint64_t x) noexcept {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

// O roteamento por partição NÃO mora aqui. Ele é decisão de FORMATO — mudá-la reordena o log de
// todas as partições e invalida todo snapshot existente — e por isso mora num lugar só, congelado
// com valores golden: `src/ingress/partitioner.hpp`. Havia uma segunda cópia da fórmula neste
// arquivo, sem nenhum chamador e sem os `static_assert` que a protegem; duas verdades sobre uma
// decisão de formato é o começo de um dia ruim.

}  // namespace rv
