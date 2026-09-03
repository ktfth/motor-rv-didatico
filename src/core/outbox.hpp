#pragma once
// O portão único de saída do núcleo — e a razão de I10 ser verificável.
//
// I10: "nenhuma saída é liberada com lsn > durable_lsn". Um invariante assim só é verificável se
// houver UM lugar por onde a saída passa. Por isso o `apply` não escreve em ring de saída, não
// loga conteúdo de negócio e não chama nada que publique: ele só empilha aqui, com o LSN do
// evento que produziu a saída. `ready(durable)` percorre da cabeça enquanto `lsn <= durable` e
// para — não existe caminho que ultrapasse.
//
// A consequência de um crash é a correta por construção: o que estava empilhado e não durável
// nunca foi visto por ninguém, então perdê-lo não perde informação. O que foi liberado é durável,
// então o replay o reproduz.

#include <cstddef>
#include <cstdint>
#include <span>

#include "base/arena.hpp"
#include "base/bytes.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"

namespace rv::core {

enum class OutKind : uint8_t {
  ReadModelUpdate = 1,   // atualização do read model intradiário
  PartitionMessage = 2,  // mensagem para outra partição
  Confirmation = 3,      // confirmação para o originador
  Audit = 4,             // trilha de auditoria (R19)
  Exception = 5,         // fila de exceção: divergência, negócio rejeitado
};

struct OutboxEntry {
  Lsn lsn;
  uint32_t offset;  // no buffer circular de payload
  uint16_t len;
  OutKind kind;
  uint8_t reserved;
};
static_assert(sizeof(OutboxEntry) == 16);

class Outbox {
 public:
  [[nodiscard]] bool init(Arena& a, uint32_t slots, uint32_t payload_bytes) noexcept;

  // Chamado de dentro do `apply`. Falha só por falta de espaço — e isso é FATAL, porque
  // significaria perder silenciosamente uma saída que o evento produziu.
  [[nodiscard]] Status stage(Lsn lsn, OutKind kind, ByteSpan payload) noexcept;

  // O prefixo máximo com `lsn <= durable`. Nunca devolve nada além disso.
  [[nodiscard]] uint32_t ready(Lsn durable) const noexcept;
  [[nodiscard]] const OutboxEntry& entry(uint32_t i) const noexcept { return slots_[(head_ + i) & mask_]; }
  [[nodiscard]] ByteSpan payload(const OutboxEntry& e) const noexcept {
    return ByteSpan{buf_ + e.offset, e.len};
  }

  void commit(uint32_t n) noexcept;  // consome n entradas já publicadas
  void freeze() noexcept { frozen_ = true; }  // após fail-stop: `ready` devolve zero para sempre
  [[nodiscard]] bool frozen() const noexcept { return frozen_; }
  [[nodiscard]] uint32_t pending() const noexcept { return count_; }
  [[nodiscard]] uint64_t staged_total() const noexcept { return staged_total_; }
  [[nodiscard]] uint64_t released_total() const noexcept { return released_total_; }

 private:
  OutboxEntry* slots_ = nullptr;
  std::byte* buf_ = nullptr;
  uint32_t mask_ = 0;
  uint32_t buf_cap_ = 0;
  uint32_t buf_head_ = 0;  // offset do próximo byte livre (circular)
  uint32_t head_ = 0;      // índice lógico da entrada mais antiga
  uint32_t count_ = 0;
  uint64_t staged_total_ = 0;
  uint64_t released_total_ = 0;
  bool frozen_ = false;
};

}  // namespace rv::core
