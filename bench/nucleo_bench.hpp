#pragma once
// O andaime de medição do núcleo: uma partição inteira, montada UMA vez e remontada entre
// repetições sem tocar o alocador do sistema.
//
// Remontar importa. Aplicar a mesma sessão duas vezes sobre o mesmo estado não mede o dobro do
// trabalho: mede o conjunto de idempotência recusando tudo na segunda volta (cenário golden 13).
// Então cada repetição começa de estado virgem — e "virgem" aqui significa `Arena` reconstruída
// sobre o MESMO bloco de memória, que continua sendo do processo desde o começo. O bloco é
// alocado no construtor, fora de qualquer cronômetro.

#include <cstddef>
#include <memory>
#include <optional>

#include "base/arena.hpp"
#include "base/metrics.hpp"
#include "core/outbox.hpp"
#include "core/partition.hpp"
#include "core/partition_state.hpp"

namespace rv::bench {

class Nucleo {
 public:
  explicit Nucleo(size_t bytes) : bytes_(bytes), memoria_(new std::byte[bytes]) {}

  // Reconstrói estado, outbox e ring de ingresso. Devolve false se a configuração não couber na
  // arena — que é o teste de dimensionamento do warm-up, rodando aqui de graça.
  [[nodiscard]] bool monta(const core::PartitionCapacity& cap, uint16_t id = 0) {
    arena_.emplace(memoria_.get(), bytes_);
    if (!arena_.has_value()) return false;  // não acontece; o analisador exige a pergunta feita
    Arena& a = *arena_;
    estado_ = core::PartitionState{};
    outbox_ = core::Outbox{};
    metricas_ = Metrics{};
    if (!estado_.init(a, PartitionId{id}, cap)) return false;
    if (!outbox_.init(a, cap.outbox_slots, cap.outbox_payload_bytes)) return false;
    entrada_ = a.emplace<core::Inbox>();
    if (entrada_ == nullptr) return false;
    a.seal();  // daqui em diante, alocar é erro — CODING_RULES §1 como mecanismo
    ctx_ = core::ApplyContext{&outbox_, &metricas_};
    return true;
  }

  [[nodiscard]] core::PartitionState& estado() noexcept { return estado_; }
  [[nodiscard]] core::Outbox& outbox() noexcept { return outbox_; }
  [[nodiscard]] Metrics& metricas() noexcept { return metricas_; }
  [[nodiscard]] core::ApplyContext& ctx() noexcept { return ctx_; }
  [[nodiscard]] core::Inbox& entrada() noexcept { return *entrada_; }

 private:
  size_t bytes_;
  std::unique_ptr<std::byte[]> memoria_;
  std::optional<Arena> arena_;
  core::PartitionState estado_;
  core::Outbox outbox_;
  Metrics metricas_;
  core::ApplyContext ctx_{nullptr, nullptr};
  core::Inbox* entrada_ = nullptr;
};

// A configuração que as medições usam. Não é a padrão de `PartitionCapacity`: o outbox precisa
// caber a sessão inteira nas medições que aplicam sem drenar por evento, e a arena precisa caber
// o outbox. Publicar a configuração junto do número é obrigatório — vazão com outbox de 16 K
// slots e vazão com outbox de 1 M slots são duas medições diferentes.
[[nodiscard]] inline core::PartitionCapacity capacidade_de_medicao() noexcept {
  core::PartitionCapacity c{};
  c.accounts = 1u << 16;
  c.instruments = 4096;
  c.positions = 1u << 18;
  c.trades = 1u << 20;
  c.outbox_slots = 1u << 16;
  c.outbox_payload_bytes = 1u << 23;
  return c;
}

inline constexpr size_t kArenaDeMedicao = 512ULL << 20;

}  // namespace rv::bench
