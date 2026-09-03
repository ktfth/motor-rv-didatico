#pragma once
// Arena de bump com `seal()`. CODING_RULES §1: sem alocação depois do warm-up.
//
// A ideia: cada partição recebe um bloco de memória no início e nunca mais pede nada ao sistema.
// `seal()` é o que transforma a regra em mecanismo — depois dele, qualquer pedido de memória é
// erro, não um `malloc` discreto que aparece como um pico de P999 seis meses depois.
//
// Por que uma arena por partição, e não uma global: shared-nothing (ADR-0005). Uma arena global
// teria de ser protegida, e um lock no caminho de alocação é exatamente o que não queremos ter.

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "base/status.hpp"

namespace rv {

class Arena {
 public:
  Arena() noexcept = default;
  Arena(std::byte* base, size_t bytes) noexcept : base_(base), cap_(bytes) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Reserva `n` bytes alinhados a `align`. Devolve nullptr quando não cabe ou quando a arena já
  // foi selada — nunca lança, nunca cresce, nunca chama o alocador do sistema.
  [[nodiscard]] void* allocate(size_t n, size_t align) noexcept {
    if (sealed_ || n == 0) return nullptr;
    const size_t off = (used_ + align - 1) & ~(align - 1);
    if (off + n > cap_) {
      overflowed_ = true;
      return nullptr;
    }
    used_ = off + n;
    ++blocks_;
    return base_ + off;
  }

  // A arena devolve memória CRUA: ela não roda construtor nem destrutor. A exigência real,
  // portanto, é que `T` seja seguro de tratar como bytes e não precise de destruição — não que
  // seu construtor padrão seja trivial.
  //
  // A primeira versão exigia `is_trivially_default_constructible`, e isso rejeitava
  // `struct Lsn { uint64_t v = 0; }` — um tipo perfeitamente POD, cujo único "pecado" é ter
  // inicializador de membro. Exigir demais de um invariante é tão ruim quanto exigir de menos:
  // empurra quem escreve o código a remover o inicializador (perdendo a inicialização segura) ou
  // a contornar a arena. Quem chama continua responsável por zerar o que alocou; `coluna()` em
  // `core/ledger.cpp` mostra o padrão.
  template <class T>
  [[nodiscard]] T* alloc_array(size_t count) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "a arena trata a memória como bytes");
    static_assert(std::is_trivially_destructible_v<T>, "a arena nunca chama destrutor");
    return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
  }

  template <class T, class... Args>
  [[nodiscard]] T* emplace(Args&&... args) noexcept {
    void* p = allocate(sizeof(T), alignof(T));
    return p ? new (p) T(static_cast<Args&&>(args)...) : nullptr;
  }

  // O fim do warm-up. Depois disto, `allocate` sempre falha — e é assim que se descobre, em
  // teste e não em produção, quem alocava no caminho quente.
  void seal() noexcept { sealed_ = true; }
  [[nodiscard]] bool sealed() const noexcept { return sealed_; }

  [[nodiscard]] size_t used() const noexcept { return used_; }
  [[nodiscard]] size_t capacity() const noexcept { return cap_; }
  [[nodiscard]] size_t remaining() const noexcept { return cap_ - used_; }
  [[nodiscard]] uint32_t blocks() const noexcept { return blocks_; }
  // Verdadeiro se algum pedido já não coube. O dimensionamento da arena é decisão de configuração,
  // e este bit é como o teste de warm-up prova que a configuração está certa.
  [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
  // O endereço base. Só o stall-and-copy usa: ele copia a arena inteira de uma vez.
  [[nodiscard]] std::byte* base() noexcept { return base_; }
  [[nodiscard]] const std::byte* base() const noexcept { return base_; }

 private:
  std::byte* base_ = nullptr;
  size_t cap_ = 0;
  size_t used_ = 0;
  uint32_t blocks_ = 0;
  bool sealed_ = false;
  bool overflowed_ = false;
};

}  // namespace rv
