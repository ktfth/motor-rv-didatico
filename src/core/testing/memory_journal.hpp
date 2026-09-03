#pragma once
// Um `Journal` em memória — o duplo de teste que torna toda a suíte do núcleo determinística.
//
// Ele não é uma simplificação preguiçosa do WAL: é o segundo cliente do `concept Journal`, e a
// existência de um segundo cliente é o que prova que a interface não vazou detalhe do primeiro.
// Além disso permite ao teste decidir EXATAMENTE quando `durable_lsn` avança, que é o que I10
// precisa e que, com io_uring de verdade, dependeria de sorte.

#include <cstring>
#include <vector>

#include "base/bytes.hpp"
#include "base/status.hpp"
#include "core/journal.hpp"

namespace rv::core::testing {

class MemoryJournal {
 public:
  struct Registro {
    Lsn lsn;
    uint64_t ts_ns;
    uint16_t tmpl;
    uint16_t len;
    uint32_t offset;
  };

  explicit MemoryJournal(size_t bytes = 16u << 20) : buf_(bytes) {}

  [[nodiscard]] Result<Appended> append(uint16_t tmpl, ByteSpan payload, uint64_t ts_ns) noexcept {
    if (parado_) return Status::fail(Err::IoError);
    if (cheio_) return Status::fail(Err::WalFull);
    const auto len = static_cast<uint16_t>(payload.size());
    // Alinha a 8 pelo mesmo motivo do WAL de verdade: o `apply` reinterpreta o payload no lugar,
    // e uma leitura desalinhada seria recusada por `view_bytes`.
    const size_t off = (usado_ + 7U) & ~size_t{7};
    if (off + len > buf_.size()) return Status::fail(Err::WalFull);
    std::memcpy(buf_.data() + off, payload.data(), len);
    usado_ = off + len;
    last_ = last_.next();
    registros_.push_back(Registro{last_, ts_ns, tmpl, len, static_cast<uint32_t>(off)});
    return Appended{last_, buf_.data() + off, len};
  }

  [[nodiscard]] Status maybe_submit(uint64_t) noexcept { return kOk; }
  [[nodiscard]] Status reap() noexcept {
    if (auto_duravel_) durable_ = last_;
    return kOk;
  }
  [[nodiscard]] Lsn durable_lsn() noexcept { return durable_; }
  [[nodiscard]] Lsn last_lsn() noexcept { return last_; }
  [[nodiscard]] bool halted() noexcept { return parado_; }

  // ---- controles que só o teste usa ----
  void set_auto_durable(bool v) noexcept { auto_duravel_ = v; }
  void advance_durable(Lsn l) noexcept { durable_ = l; }
  void set_full(bool v) noexcept { cheio_ = v; }
  void halt() noexcept { parado_ = true; }

  [[nodiscard]] const std::vector<Registro>& records() const noexcept { return registros_; }
  [[nodiscard]] ByteSpan payload_of(const Registro& r) const noexcept {
    return ByteSpan{buf_.data() + r.offset, r.len};
  }

 private:
  std::vector<std::byte> buf_;
  std::vector<Registro> registros_;
  size_t usado_ = 0;
  Lsn last_{};
  Lsn durable_{};
  bool auto_duravel_ = true;
  bool cheio_ = false;
  bool parado_ = false;
};

static_assert(Journal<MemoryJournal>, "o duplo de teste tem de satisfazer o MESMO contrato");

}  // namespace rv::core::testing
