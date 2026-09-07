#pragma once
// O `Journal` da medição: zero alocação, durabilidade com ATRASO PROGRAMÁVEL.
//
// Por que não usar o `MemoryJournal` de `core/testing/`: ele guarda um `std::vector<Registro>` e
// dá `push_back` a cada `append`. Num teste isso é irrelevante; numa medição de eventos por
// segundo, é o alocador do sistema entrando no meio do número — exatamente o que a arena selada
// existe para impedir (CODING_RULES §1). Medir o motor com um duplo que aloca seria publicar a
// latência do `malloc` como se fosse a do `apply`.
//
// Por que ele é o TERCEIRO cliente do `concept Journal`: dois clientes provam que a interface não
// vazou detalhe do primeiro; três, escrito por outro papel e com outra restrição (não alocar),
// prova que ela também não vazou detalhe do teste.
//
// O ATRASO existe para medir o que o motor faz de verdade sob group commit: `durable_lsn` fica
// N appends atrás de `last_lsn`, então o outbox acumula e o loop paga o `ready`/`commit` de um
// prefixo, e não de uma entrada por volta. Com atraso zero, mede-se um regime que o WAL nunca
// entrega.

#include <cstring>

#include "base/bytes.hpp"
#include "base/status.hpp"
#include "core/journal.hpp"

namespace rv::bench {

class BenchJournal {
 public:
  // O buffer é do chamador e vem de fora da região medida. Circular: o `apply` lê o payload
  // imediatamente depois do `append` e nunca depois disso, então reaproveitar é seguro — é a
  // mesma vida útil que o buffer de commit do WAL de verdade dá ("válido até o próximo
  // `maybe_submit`", em core/journal.hpp).
  BenchJournal(std::byte* buf, size_t bytes) noexcept : buf_(buf), cap_(bytes) {}

  [[nodiscard]] Result<core::Appended> append(uint16_t /*tmpl*/, ByteSpan payload,
                                              uint64_t /*ts_ns*/) noexcept {
    const auto len = static_cast<uint16_t>(payload.size());
    size_t off = (usado_ + 7U) & ~size_t{7};
    if (off + len > cap_) off = 0;  // volta ao começo; nada pendente aponta para trás
    std::memcpy(buf_ + off, payload.data(), len);
    usado_ = off + len;
    last_ = last_.next();
    return core::Appended{last_, buf_ + off, len};
  }

  [[nodiscard]] Status maybe_submit(uint64_t) noexcept { return kOk; }

  [[nodiscard]] Status reap() noexcept {
    durable_ = (last_.v > atraso_) ? Lsn{last_.v - atraso_} : Lsn{};
    return kOk;
  }

  [[nodiscard]] Lsn durable_lsn() noexcept { return durable_; }
  [[nodiscard]] Lsn last_lsn() noexcept { return last_; }
  [[nodiscard]] bool halted() noexcept { return false; }

  void set_atraso(uint64_t n) noexcept { atraso_ = n; }
  void reinicia() noexcept {
    usado_ = 0;
    last_ = Lsn{};
    durable_ = Lsn{};
  }

 private:
  std::byte* buf_ = nullptr;
  size_t cap_ = 0;
  size_t usado_ = 0;
  uint64_t atraso_ = 0;
  Lsn last_{};
  Lsn durable_{};
};

static_assert(core::Journal<BenchJournal>,
              "o journal de medição tem de satisfazer o MESMO contrato do WAL");

}  // namespace rv::bench
