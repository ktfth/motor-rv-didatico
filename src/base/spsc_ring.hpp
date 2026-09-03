#pragma once
// Fila de um produtor e um consumidor, estilo Disruptor. É o único ponto do motor com atomics
// (CODING_RULES §5) — e são exatamente dois.
//
// Três decisões, todas com o mesmo motivo (a linha de cache):
//   1. Os dois cursores ficam em linhas de cache SEPARADAS. Juntos, o produtor escrevendo `head`
//      invalidaria a linha que o consumidor lê para consultar `tail`, e vice-versa: cada
//      publicação custaria uma transferência de linha entre cores.
//   2. Cada lado guarda uma CÓPIA do cursor do outro. O consumidor só relê `head` do produtor
//      quando sua cópia diz que a fila está vazia. Numa fila com trabalho acumulado — o caso
//      comum sob carga — o cursor remoto é lido uma vez por lote, não uma vez por item.
//   3. A capacidade é potência de dois, então o índice é uma máscara e não um resto.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace rv {

inline constexpr size_t kCacheLine = 64;

template <class T, size_t Capacity>
class SpscRing {
  static_assert((Capacity & (Capacity - 1)) == 0, "capacidade tem de ser potência de dois");
  static_assert(std::is_trivially_copyable_v<T>, "o slot é POD: publicar é escrever, não construir");

 public:
  static constexpr size_t kCapacity = Capacity;
  static constexpr size_t kMask = Capacity - 1;

  // ---- lado do produtor ----

  // Endereço do próximo slot livre, ou nullptr se cheio. O produtor escreve NO LUGAR e depois
  // chama `publish()` — sem cópia intermediária.
  [[nodiscard]] T* claim() noexcept {
    const uint64_t h = head_.value.load(std::memory_order_relaxed);
    if (h - tail_cache_ >= Capacity) {
      tail_cache_ = tail_.value.load(std::memory_order_acquire);
      if (h - tail_cache_ >= Capacity) return nullptr;
    }
    return &slots_[h & kMask];
  }

  // Torna visível o slot escrito. `release` é o que garante que o consumidor, ao ver o novo
  // `head`, enxergue também os bytes do slot.
  void publish() noexcept {
    head_.value.store(head_.value.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  // ---- lado do consumidor ----

  [[nodiscard]] const T* peek() noexcept {
    const uint64_t t = tail_.value.load(std::memory_order_relaxed);
    if (t >= head_cache_) {
      head_cache_ = head_.value.load(std::memory_order_acquire);
      if (t >= head_cache_) return nullptr;
    }
    return &slots_[t & kMask];
  }

  void pop() noexcept {
    tail_.value.store(tail_.value.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

  // Quantos itens estão disponíveis agora, sem reler o cursor remoto. Serve para a drenagem em
  // lote do loop da partição decidir o tamanho do lote.
  [[nodiscard]] uint64_t available() noexcept {
    head_cache_ = head_.value.load(std::memory_order_acquire);
    return head_cache_ - tail_.value.load(std::memory_order_relaxed);
  }

  [[nodiscard]] uint64_t produced() const noexcept {
    return head_.value.load(std::memory_order_relaxed);
  }
  [[nodiscard]] uint64_t consumed() const noexcept {
    return tail_.value.load(std::memory_order_relaxed);
  }

 private:
  struct alignas(kCacheLine) Cursor {
    std::atomic<uint64_t> value{0};
  };
  static_assert(sizeof(Cursor) == kCacheLine);
  static_assert(std::atomic<uint64_t>::is_always_lock_free);

  Cursor head_;                  // só o produtor escreve
  Cursor tail_;                  // só o consumidor escreve
  alignas(kCacheLine) uint64_t tail_cache_ = 0;  // cópia do produtor
  alignas(kCacheLine) uint64_t head_cache_ = 0;  // cópia do consumidor
  alignas(kCacheLine) T slots_[Capacity]{};
};

}  // namespace rv
