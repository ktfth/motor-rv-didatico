#pragma once
// Erro sem exceção. CODING_RULES §4.
//
// `Status` cabe em oito bytes e é trivialmente copiável: devolver um erro custa o mesmo que
// devolver um `int`. Não há alocação, não há `std::string`, não há hierarquia de tipos.
//
// O número de cada `Err` é ESTÁVEL e agrupado por faixa. Ele aparece em log, em métrica e no
// relatório de fail-stop, e `grep 252` acha a causa sem consultar tabela nenhuma. Números não são
// reordenados quando um valor novo entra — entra no fim da faixa.

#include <cstdint>
#include <type_traits>

namespace rv {

enum class Err : uint16_t {
  Ok = 0,

  // 1..99 — base
  OutOfRange = 1,
  ArenaExhausted = 2,
  Overflow = 3,
  NotFound = 4,
  WouldBlock = 5,
  InvalidArgument = 6,

  // 100..199 — codec
  UnknownTemplate = 100,
  ShortPayload = 101,
  BadBlockLength = 102,
  GroupTooLarge = 103,
  Misaligned = 104,

  // 200..299 — núcleo e domínio.
  // 200..249 são REJEIÇÕES de negócio; 250..299 são FATAIS. A faixa carrega a classificação,
  // então `is_fatal` é uma comparação e não uma tabela que alguém esquece de atualizar.
  InvalidTransition = 200,  // I5
  AlreadyApplied = 201,     // I6 — idempotência
  NegativeBucket = 202,     // I3
  QtyMismatch = 203,
  UnknownBatch = 204,
  AmountMismatch = 205,     // provento cujo bruto − irrf ≠ líquido (cenário golden 10)
  ShortSaleNotAllowed = 206,
  // O instrumento existe no log mas ainda não foi DESCRITO (`ClosingPriceSet` não chegou), então
  // o motor não conhece o fator de cotação. Negociar assim produziria dinheiro errado. É rejeição,
  // não fatal: uma lacuna no arquivo de cadastro da B3 não pode derrubar a partição de um core
  // inteiro — vira exceção registrada, e o negócio é reprocessado quando o cadastro chegar.
  InstrumentNotDescribed = 207,
  // Data de liquidação fora da janela D+0..D+2. Evento atrasado ou fora de ordem.
  OutsideSettlementWindow = 208,

  UnknownInstrument = 250,  // fatal: quebra o contrato de ordenação do log
  UnknownAccount = 251,     // fatal: idem
  LedgerOverflow = 252,     // fatal: int64 estourou num bucket
  StateCorrupt = 253,       // fatal

  // 300..399 — WAL. 300..349 transitórias; 350..399 fatais.
  WalFull = 300,            // sem buffer livre ou kMaxInflight atingido: tente na próxima volta
  ShortWrite = 350,
  IoError = 351,
  BadCrc = 352,
  BadMagic = 353,
  LsnGap = 354,
  EpochMismatch = 355,
  SegmentFull = 356,

  // 400..499 — borda
  MissingInteractionId = 400,
  BadSignature = 401,
  TokenExpired = 402,
  CertBindingMismatch = 403,
  ConsentNotAuthorised = 404,
  ScopeMissing = 405,
  OperationalLimit = 406,
  RateLimited = 407,
  MalformedRequest = 408,
  ResourceNotFound = 409,
  TlsProfileViolation = 410,
};

[[nodiscard]] const char* to_string(Err) noexcept;

// Um erro é fatal quando continuar significaria corromper: o ledger, o log ou o estado. Erro de
// entrada externa nunca é fatal — derrubar a partição porque um terceiro mandou lixo transforma
// o erro dele em indisponibilidade nossa.
//
// A regra é a FAIXA, mais uma lista curta e explícita. A primeira versão era só faixa, e o texto
// de `core/apply.hpp` — que classifica "arena sem espaço" como fatal — discordava do número:
// `ArenaExhausted` vale 2, cai em 1..99, e era tratado como rejeição. O efeito prático de ficar
// sem arena e apenas "rejeitar" o evento é que a partição continua rodando com estado que ela não
// conseguiu registrar, e os checksums de `EodMarked` passariam sem acusar nada.
//
// Duas fontes para a mesma decisão é o defeito; a lista explícita ao lado da faixa é a correção
// honesta — a alternativa seria renumerar `ArenaExhausted`, e número de `Err` aparece em log e em
// relatório de crash, logo é estável por contrato.
[[nodiscard]] constexpr bool is_fatal(Err e) noexcept {
  const auto v = static_cast<uint16_t>(e);
  if (v >= 250 && v < 300) return true;   // núcleo: corrupção de estado ou de ordenação do log
  if (v >= 350 && v < 400) return true;   // WAL: durabilidade comprometida
  return e == Err::ArenaExhausted;        // ficar sem memória depois do warm-up é fatal
}
static_assert(is_fatal(Err::ArenaExhausted));
static_assert(is_fatal(Err::LedgerOverflow) && is_fatal(Err::StateCorrupt));
static_assert(is_fatal(Err::BadCrc) && is_fatal(Err::ShortWrite));
static_assert(!is_fatal(Err::NegativeBucket) && !is_fatal(Err::ShortSaleNotAllowed));
static_assert(!is_fatal(Err::WalFull), "contrapressão não é corrupção");
static_assert(!is_fatal(Err::UnknownTemplate),
              "template desconhecido pode vir do ingress; quem trata log corrompido é a recuperação");

class [[nodiscard]] Status {
 public:
  constexpr Status() noexcept = default;

  static constexpr Status fail(Err e, uint32_t detail = 0) noexcept { return Status{e, detail}; }

  [[nodiscard]] constexpr bool is_ok() const noexcept { return code_ == Err::Ok; }
  [[nodiscard]] constexpr bool is_error() const noexcept { return code_ != Err::Ok; }
  [[nodiscard]] constexpr Err code() const noexcept { return code_; }

  // Um número livre para o contexto: LSN curto, offset no segmento, índice do bucket. Não é
  // mensagem — é o que se precisa para reproduzir, e cabe no mesmo registrador.
  [[nodiscard]] constexpr uint32_t detail() const noexcept { return detail_; }
  [[nodiscard]] constexpr bool is_fatal() const noexcept { return rv::is_fatal(code_); }

  friend constexpr bool operator==(Status, Status) noexcept = default;

 private:
  constexpr Status(Err e, uint32_t d) noexcept : code_(e), detail_(d) {}
  Err code_{Err::Ok};
  uint16_t pad_{0};
  uint32_t detail_{0};
};
static_assert(sizeof(Status) == 8);
static_assert(std::is_trivially_copyable_v<Status>);

inline constexpr Status kOk{};

// `Result<T>` só aceita T trivialmente copiável: é o tipo de retorno do caminho quente, e um
// destrutor não trivial ali dentro significaria que alguém pôs um dono de recurso onde não cabe.
template <class T>
class [[nodiscard]] Result {
  static_assert(std::is_trivially_copyable_v<T>, "Result<T> é para valores POD do hot path");

 public:
  constexpr Result(T v) noexcept : v_(v) {}                      // NOLINT: conversão desejada
  constexpr Result(Status s) noexcept : st_(s), v_{} {}          // NOLINT: idem

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return st_.is_ok(); }
  [[nodiscard]] constexpr bool is_ok() const noexcept { return st_.is_ok(); }
  [[nodiscard]] constexpr Status status() const noexcept { return st_; }

  [[nodiscard]] constexpr const T& operator*() const noexcept { return v_; }
  [[nodiscard]] constexpr const T* operator->() const noexcept { return &v_; }
  [[nodiscard]] constexpr T value_or(T fallback) const noexcept { return st_.is_ok() ? v_ : fallback; }

 private:
  Status st_{};
  T v_;
};

}  // namespace rv
