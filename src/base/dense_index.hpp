#pragma once
// Índice de endereçamento aberto de `uint64` para `uint32`. É o que transforma um documento numa
// linha da coluna SoA, e uma chave `(action_id, conta)` numa resposta de idempotência (I6).
//
// Endereçamento aberto com sondagem linear, e não encadeamento, por um motivo só: o caminho de
// busca toca uma linha de cache e, no caso comum, uma só. Encadeamento tocaria uma por nó.
//
// A capacidade é potência de dois e o fator de carga máximo é 0,7. A tabela NUNCA cresce: o
// tamanho é decidido no warm-up, e ficar sem espaço é erro de configuração, detectado no teste de
// dimensionamento e não em produção. Crescer exigiria realocar — e a arena está selada.

#include <cstddef>
#include <cstdint>

#include "base/arena.hpp"
#include "base/ids.hpp"

namespace rv {

class DenseIndex {
 public:
  static constexpr uint32_t kEmpty = 0xFFFFFFFFU;

  // `max_elements` é quantos elementos o índice precisa GUARDAR — não o tamanho da tabela.
  // A tabela é dimensionada para que essa quantidade caiba sob o fator de carga.
  //
  // A primeira versão desta função recebia o tamanho da tabela e deixava a divisão por 0,7 para
  // quem chamava. Um teste que pediu 1000 e conseguiu guardar 716 mostrou por que isso é uma API
  // ruim: o erro é silencioso, aparece só quando a tabela enche, e enche em produção. Quem chama
  // sabe quantas contas espera; quantos slots isso exige é problema daqui.
  [[nodiscard]] bool init(Arena& arena, uint32_t max_elements) noexcept {
    const uint64_t minimo = (static_cast<uint64_t>(max_elements) * 10 + 6) / 7;  // /0,7 arredondado
    uint32_t cap = 16;
    while (cap < minimo) cap <<= 1U;
    keys_ = arena.alloc_array<uint64_t>(cap);
    vals_ = arena.alloc_array<uint32_t>(cap);
    if (keys_ == nullptr || vals_ == nullptr) return false;
    cap_ = cap;
    mask_ = cap - 1;
    for (uint32_t i = 0; i < cap_; ++i) {
      keys_[i] = 0;
      vals_[i] = kEmpty;
    }
    size_ = 0;
    return true;
  }

  // Insere ou devolve o valor já presente. `inserted` diz qual dos dois aconteceu — é o que faz
  // esta função servir tanto para internar um documento quanto para responder I6.
  [[nodiscard]] uint32_t insert_or_get(uint64_t key, uint32_t value, bool& inserted) noexcept {
    uint32_t i = static_cast<uint32_t>(mix64(key)) & mask_;
    for (uint32_t passos = 0; passos <= mask_; ++passos) {
      if (vals_[i] == kEmpty) {
        if (size_ * 10 >= static_cast<uint64_t>(cap_) * 7) {  // carga > 0,7
          inserted = false;
          return kEmpty;
        }
        keys_[i] = key;
        vals_[i] = value;
        ++size_;
        inserted = true;
        return value;
      }
      if (keys_[i] == key) {
        inserted = false;
        return vals_[i];
      }
      i = (i + 1) & mask_;
    }
    inserted = false;
    return kEmpty;
  }

  [[nodiscard]] uint32_t find(uint64_t key) const noexcept {
    uint32_t i = static_cast<uint32_t>(mix64(key)) & mask_;
    for (uint32_t passos = 0; passos <= mask_; ++passos) {
      if (vals_[i] == kEmpty) return kEmpty;
      if (keys_[i] == key) return vals_[i];
      i = (i + 1) & mask_;
    }
    return kEmpty;
  }

  [[nodiscard]] bool contains(uint64_t key) const noexcept { return find(key) != kEmpty; }
  [[nodiscard]] uint32_t size() const noexcept { return size_; }
  // Quantos elementos ainda cabem sob o fator de carga — o número que interessa a quem dimensiona.
  [[nodiscard]] uint32_t capacity() const noexcept {
    return static_cast<uint32_t>(static_cast<uint64_t>(cap_) * 7 / 10);
  }
  // O tamanho da tabela. Só o teste de dimensionamento e a métrica de memória olham para isto.
  [[nodiscard]] uint32_t slots() const noexcept { return cap_; }

  // Percorre em ordem de SLOT, não de inserção. Quem precisa de ordem determinística (o escritor
  // do snapshot, por exemplo) percorre a coluna SoA, que é densa e ordenada por índice — nunca
  // esta tabela. Iterar tabela de hash e depender da ordem é a violação clássica de D4.
  template <class F>
  void for_each_slot(F&& f) const noexcept {
    for (uint32_t i = 0; i < cap_; ++i) {
      if (vals_[i] != kEmpty) f(keys_[i], vals_[i]);
    }
  }

 private:
  uint64_t* keys_ = nullptr;
  uint32_t* vals_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t mask_ = 0;
  uint32_t size_ = 0;
};

}  // namespace rv
