#pragma once
// Contadores e histogramas de tamanho fixo. Zero alocação, zero relógio.
//
// O histograma é log-linear: 64 baldes por oitava. Ele guarda a FORMA da distribuição com erro
// relativo limitado (~1,5 %) em qualquer ordem de grandeza, usando 4 KiB fixos. É isso que
// permite ter P50, P99 e P999 de latência sem guardar amostra nenhuma — e sem alocar no caminho
// que estamos justamente medindo.

#include <bit>
#include <cstdint>

namespace rv {

class Histogram {
 public:
  static constexpr uint32_t kSubBuckets = 64;   // por oitava
  static constexpr uint32_t kOctaves = 40;      // até ~2^40 ns ≈ 18 minutos
  static constexpr uint32_t kBuckets = kSubBuckets * kOctaves;

  void record(uint64_t v) noexcept {
    ++count_;
    sum_ += v;
    if (v > max_) max_ = v;
    buckets_[index_of(v)]++;
  }

  [[nodiscard]] uint64_t count() const noexcept { return count_; }
  [[nodiscard]] uint64_t sum() const noexcept { return sum_; }
  [[nodiscard]] uint64_t max() const noexcept { return max_; }

  // O valor abaixo do qual está a fração `q` das amostras. Percorre 2560 baldes: barato o
  // bastante para chamar no fim de um bench, caro demais para chamar no caminho quente — e é por
  // isso que `record` e `quantile` são funções separadas.
  [[nodiscard]] uint64_t quantile(double q) const noexcept {
    if (count_ == 0) return 0;
    const uint64_t alvo = static_cast<uint64_t>(q * static_cast<double>(count_));
    uint64_t acc = 0;
    for (uint32_t i = 0; i < kBuckets; ++i) {
      acc += buckets_[i];
      if (acc > alvo) return value_of(i);
    }
    return max_;
  }

  void reset() noexcept {
    count_ = sum_ = max_ = 0;
    for (auto& b : buckets_) b = 0;
  }

 private:
  // Índice: oitava × 64 + posição dentro da oitava. Para v < 64 o índice é o próprio v, o que
  // dá resolução de 1 ns nos valores pequenos — onde a latência de append vive.
  [[nodiscard]] static uint32_t index_of(uint64_t v) noexcept {
    if (v < kSubBuckets) return static_cast<uint32_t>(v);
    const uint32_t oitava = 63U - static_cast<uint32_t>(std::countl_zero(v));
    const uint32_t desl = oitava - 6U;  // log2(kSubBuckets)
    const uint32_t sub = static_cast<uint32_t>((v >> desl) & (kSubBuckets - 1));
    const uint32_t i = (desl + 1U) * kSubBuckets + sub;
    return i < kBuckets ? i : kBuckets - 1;
  }
  [[nodiscard]] static uint64_t value_of(uint32_t i) noexcept {
    if (i < kSubBuckets) return i;
    const uint32_t oitava = i / kSubBuckets;
    const uint32_t sub = i % kSubBuckets;
    const uint32_t desl = oitava - 1U;
    return (static_cast<uint64_t>(kSubBuckets) + sub) << desl;
  }

  uint64_t buckets_[kBuckets]{};
  uint64_t count_ = 0;
  uint64_t sum_ = 0;
  uint64_t max_ = 0;
};

// Contadores do motor. Um struct chato de propósito: nome do campo é o nome da métrica, e não há
// registro dinâmico, string nem mapa. O que se ganha com registro dinâmico — descobrir métricas
// em runtime — não vale um `find` de string no caminho quente.
class Metrics {
 public:
  // núcleo
  uint64_t apply_accepted = 0;
  uint64_t apply_rejected = 0;
  uint64_t apply_fatal = 0;
  uint64_t trades_closed = 0;  // negócios baixados e compactados na virada do dia

  // Contagem EXATA por código, indexada pelo próprio número do `Err`. A primeira versão indexava
  // por (código − 200) num vetor de 16, e a simulação mostrou o problema na hora: metade das
  // rejeições não aparecia no relatório, porque `NotFound` (4) e `ShortPayload` (101) estão fora
  // daquela faixa. Um relatório de rejeição incompleto é pior que nenhum — ele dá a impressão de
  // que a lista está fechada. Dois quilobytes por partição resolvem; é o preço de uma linha de
  // cache por código, uma vez por rejeição.
  static constexpr uint16_t kMaxErrCode = 512;
  uint32_t rejected_by_code[kMaxErrCode]{};

  // WAL
  uint64_t wal_appends = 0;
  uint64_t wal_groups = 0;
  uint64_t wal_bytes = 0;
  uint64_t wal_full = 0;
  Histogram group_bytes;
  Histogram inflight;
  Histogram durable_latency_ns;

  // saída
  uint64_t outbox_staged = 0;
  uint64_t outbox_released = 0;
  uint64_t outbox_full = 0;  // quantas vezes o loop parou por contrapressão da saída

  void count_reject(uint16_t err_code) noexcept {
    ++apply_rejected;
    if (err_code < kMaxErrCode) ++rejected_by_code[err_code];
  }
};

}  // namespace rv
