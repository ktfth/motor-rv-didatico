#pragma once
// O backend que MENTE, de propósito e sob controle. ADR-0023.
//
// I9 (`durable_lsn` avança só em ordem FIFO), I10 (nada sai do outbox acima de `durable_lsn`) e a
// regra de fail-stop de docs/wal.md ("erro ou short write em CQE") descrevem o que acontece
// quando o disco se comporta mal. Com hardware de verdade, esperar por esse comportamento é
// esperar por sorte: o dispositivo da máquina de referência completa em ordem e nunca escreve
// menos do que foi pedido. Um invariante que só é exercitado por acaso não está testado.
//
// Por isso este arquivo é DECORADOR e não backend próprio: ele embrulha qualquer `IoBackend` —
// inclusive o de io_uring — e altera o que o WAL VÊ. As quatro falhas que ele fabrica são as
// quatro que docs/wal.md descreve:
//
//   escrita curta  → encurta a escrita DE VERDADE (o disco fica com o grupo pela metade) e ainda
//                    assim anuncia `expected` como o tamanho pedido, que é o que o CQE faria.
//   erro de CQE    → `res = -errno` sem tocar no arquivo.
//   reordenação    → entrega a completion mais nova primeiro.
//   completion     → segura a notificação de um grupo indefinidamente. É a simulação da queda no
//   segurada        meio de um grupo: o WAL nunca souber que N terminou tem de ser indistinguível,
//                    do lado do outbox, de N não ter terminado.
//
// Mora em `src/wal/testing/` e não em `tests/` porque é parte do CONTRATO do backend: quem
// escrever um backend novo (SPDK, io_uring com buffer ring) tem de passar na mesma suíte, e a
// suíte precisa deste arquivo compilando junto com a biblioteca.

#include <cstdint>
#include <span>

#include "wal/io_backend.hpp"

namespace rv::wal::testing {

class FaultBackend final : public IoBackend {
 public:
  enum class Delivery : uint8_t {
    Fifo = 0,  // ordem de chegada
    Lifo = 1,  // a mais nova primeiro — a inversão que I9 tem de sobreviver
  };

  explicit FaultBackend(IoBackend& inner) noexcept : inner_(inner) {}

  [[nodiscard]] const char* name() const noexcept override { return "fault"; }

  [[nodiscard]] Status register_files(std::span<const int> fds) noexcept override {
    return inner_.register_files(fds);
  }
  [[nodiscard]] Status register_buffers(std::span<const MutBytes> bufs) noexcept override {
    return inner_.register_buffers(bufs);
  }

  // ------------------------------------------------------------------------- injeção
  // Todas as regras são POR TOKEN (o `last_lsn` do grupo), armadas antes do `submit` e consumidas
  // uma única vez. Indexar por token e não por ordem de chegada é o que permite escrever o teste
  // como "o grupo 3 falha", que é como o cenário é descrito em docs/wal.md — e não como "a
  // terceira submissão falha", que obriga quem lê o teste a contar submissões.

  // Escreve só `bytes` do pedido. Use múltiplo do bloco quando o backend interno for O_DIRECT.
  void inject_short_write(uint64_t token, uint32_t bytes) noexcept {
    arm(token, Kind::Short, bytes);
  }
  // `err` positivo (ex.: EIO); é reportado como `-err`, exatamente como o CQE faria.
  void inject_error(uint64_t token, int32_t err) noexcept {
    arm(token, Kind::Error, static_cast<uint32_t>(err < 0 ? -err : err));
  }
  // Segura a completion deste grupo até `release_held()`. O disco recebe a escrita.
  void hold(uint64_t token) noexcept { arm(token, Kind::Hold, 0); }

  // Libera o que já foi colhido E o que ainda está no backend interno: sem a segunda metade,
  // um `release_held()` chamado antes do `reap` não teria efeito nenhum sobre um pedido que ainda
  // não voltou — e o teste passaria a depender da ordem em que ele chama as duas funções.
  void release_held() noexcept {
    for (uint32_t i = 0; i < n_pend_; ++i) pend_[i].held = false;
    for (uint32_t i = 0; i < n_fix_; ++i) fix_[i].hold = false;
  }
  void set_delivery(Delivery d) noexcept { delivery_ = d; }
  void clear_rules() noexcept { n_rules_ = 0; }

  [[nodiscard]] uint32_t held_count() const noexcept {
    uint32_t n = 0;
    for (uint32_t i = 0; i < n_pend_; ++i) n += pend_[i].held ? 1u : 0u;
    return n;
  }

  // ------------------------------------------------------------------------- IoBackend
  [[nodiscard]] Status submit(const WriteRequest& req) noexcept override {
    if (n_pend_ >= kPend || n_fix_ >= kPend) return Status::fail(Err::WalFull, n_pend_);
    const Rule* r = consume(req.token);

    if (r != nullptr && r->kind == Kind::Error) {
      // Nada vai ao disco. É o caso em que o dispositivo recusa antes de gravar — e o do grupo
      // que precisa ser truncado na recuperação porque nada além dele foi externalizado.
      push(Completion{req.token, -static_cast<int32_t>(r->arg), req.len}, false);
      return kOk;
    }

    WriteRequest efetivo = req;
    uint32_t anunciado = req.len;
    if (r != nullptr && r->kind == Kind::Short) {
      efetivo.len = r->arg < req.len ? r->arg : req.len;
      // `anunciado` continua sendo o que o WAL pediu: é assim que o CQE aparece de verdade, e é
      // o que faz `Completion::short_write()` ser verdadeiro em vez de "escrita completa menor".
    }

    const Status st = inner_.submit(efetivo);
    if (st.is_error()) return st;
    fix_[n_fix_++] = Fixup{req.token, anunciado, r != nullptr && r->kind == Kind::Hold};
    return kOk;
  }

  [[nodiscard]] uint32_t reap(std::span<Completion> out) noexcept override {
    drenar_interno();

    uint32_t n = 0;
    while (n < out.size()) {
      const uint32_t idx = escolher();
      if (idx == kPend) break;
      out[n++] = pend_[idx].c;
      remover(idx);
    }
    return n;
  }

  [[nodiscard]] uint32_t inflight() const noexcept override {
    return inner_.inflight() + n_pend_;
  }

 private:
  enum class Kind : uint8_t { Short, Error, Hold };

  struct Rule {
    uint64_t token = 0;
    Kind kind = Kind::Short;
    uint32_t arg = 0;
  };
  struct Fixup {
    uint64_t token = 0;
    uint32_t announced = 0;
    bool hold = false;
  };
  struct Pend {
    Completion c{};
    bool held = false;
  };

  static constexpr uint32_t kRules = 16;
  static constexpr uint32_t kPend = kMaxInflight * 2;

  void arm(uint64_t token, Kind k, uint32_t arg) noexcept {
    if (n_rules_ < kRules) rules_[n_rules_++] = Rule{token, k, arg};
  }

  [[nodiscard]] const Rule* consume(uint64_t token) noexcept {
    for (uint32_t i = 0; i < n_rules_; ++i) {
      if (rules_[i].token == token) {
        achado_ = rules_[i];
        rules_[i] = rules_[n_rules_ - 1];
        --n_rules_;
        return &achado_;
      }
    }
    return nullptr;
  }

  void push(Completion c, bool held) noexcept {
    if (n_pend_ < kPend) pend_[n_pend_++] = Pend{c, held};
  }

  void drenar_interno() noexcept {
    Completion buf[kMaxInflight]{};
    const uint32_t n = inner_.reap(std::span<Completion>{buf, kMaxInflight});
    for (uint32_t i = 0; i < n; ++i) {
      Completion c = buf[i];
      bool held = false;
      for (uint32_t k = 0; k < n_fix_; ++k) {
        if (fix_[k].token == c.token) {
          c.expected = fix_[k].announced;  // ver `inject_short_write`
          held = fix_[k].hold;
          fix_[k] = fix_[n_fix_ - 1];
          --n_fix_;
          break;
        }
      }
      push(c, held);
    }
  }

  [[nodiscard]] uint32_t escolher() const noexcept {
    if (delivery_ == Delivery::Fifo) {
      for (uint32_t i = 0; i < n_pend_; ++i) {
        if (!pend_[i].held) return i;
      }
    } else {
      for (uint32_t i = n_pend_; i > 0; --i) {
        if (!pend_[i - 1].held) return i - 1;
      }
    }
    return kPend;
  }

  void remover(uint32_t idx) noexcept {
    for (uint32_t i = idx; i + 1 < n_pend_; ++i) pend_[i] = pend_[i + 1];
    --n_pend_;
  }

  IoBackend& inner_;
  Rule rules_[kRules]{};
  Rule achado_{};
  uint32_t n_rules_ = 0;
  Fixup fix_[kPend]{};
  uint32_t n_fix_ = 0;
  Pend pend_[kPend]{};
  uint32_t n_pend_ = 0;
  Delivery delivery_ = Delivery::Fifo;
};

}  // namespace rv::wal::testing
