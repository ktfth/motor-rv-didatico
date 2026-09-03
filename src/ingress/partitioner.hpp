#pragma once
// Para qual core vai cada investidor. ADR-0005.
//
// A regra é uma linha — `partition = mix64(documento) & (n − 1)` — e ela é FORMATO, não detalhe:
// mudá-la muda para qual core vai cada investidor, o que reordena o log de todas as partições e
// invalida todo snapshot existente. Por isso `mix64` mora congelada em `base/ids.hpp`, com teste
// golden de valores fixos, e por isso este arquivo é tão curto.
//
// Por que por DOCUMENTO e não por instrumento ou por negócio: porque toda a história de um
// investidor precisa viver num único core. Um negócio toca a custódia (conta × instrumento) e o
// financeiro (conta): particionar por instrumento espalharia o financeiro de uma conta por vários
// cores e exigiria coordenação — exatamente o que shared-nothing existe para evitar.
//
// O custo assumido: rebalancear (mudar `n`) exige replay do log das partições afetadas. É caro e
// raro, e a alternativa — um mapa de roteamento mutável — colocaria uma indireção e uma fonte de
// não-determinismo no caminho de entrada.

#include <cstdint>

#include "base/ids.hpp"

namespace rv::ingress {

class Partitioner {
 public:
  // `n` tem de ser potência de dois: a máscara evita a divisão no caminho de entrada.
  explicit constexpr Partitioner(uint32_t n) noexcept : n_(n), mask_(n - 1) {}

  [[nodiscard]] constexpr PartitionId of(DocumentId doc) const noexcept {
    return PartitionId{static_cast<uint16_t>(mix64(doc.v) & mask_)};
  }
  [[nodiscard]] constexpr uint32_t count() const noexcept { return n_; }

  [[nodiscard]] static constexpr bool is_valid_count(uint32_t n) noexcept {
    return n > 0 && (n & (n - 1)) == 0 && n <= 65536;
  }

 private:
  uint32_t n_;
  uint32_t mask_;
};

// Valores fixos: se `mix64` mudar, estes `static_assert` quebram antes de qualquer snapshot ser
// invalidado em silêncio.
static_assert(Partitioner{4}.of(cpf(11122233344ULL)).v == 2);
static_assert(Partitioner{8}.of(cpf(11122233344ULL)).v == 6);
static_assert(Partitioner{1}.of(cpf(99999999999ULL)).v == 0);
static_assert(Partitioner::is_valid_count(4) && !Partitioner::is_valid_count(6));

}  // namespace rv::ingress
