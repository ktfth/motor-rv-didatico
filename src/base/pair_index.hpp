#pragma once
// Índice de endereçamento aberto com chave de PAR (uint64, uint32) — exato, sem colisão possível.
//
// Por que não usar `DenseIndex` misturando os dois num `uint64`: o par (action_id, conta) é a
// chave de idempotência de evento corporativo (I6). Misturar 96 bits em 64 cria uma chance
// pequena, mas real, de duas chaves diferentes virarem a mesma — e o efeito seria PULAR um evento
// corporativo legítimo de um investidor. "Pequena chance de errar dinheiro" não é um trade-off
// aceitável quando o custo de acertar é guardar quatro bytes a mais por entrada.
//
// A mistura entra só na FUNÇÃO DE HASH; a comparação usa os dois campos.

#include <cstdint>

#include "base/arena.hpp"
#include "base/ids.hpp"

namespace rv {

class PairIndex {
 public:
  [[nodiscard]] bool init(Arena& arena, uint32_t max_elements) noexcept {
    const uint64_t minimo = (static_cast<uint64_t>(max_elements) * 10 + 6) / 7;
    uint32_t cap = 16;
    while (cap < minimo) cap <<= 1U;
    hi_ = arena.alloc_array<uint64_t>(cap);
    lo_ = arena.alloc_array<uint32_t>(cap);
    ocupado_ = arena.alloc_array<uint8_t>(cap);
    if (hi_ == nullptr || lo_ == nullptr || ocupado_ == nullptr) return false;
    cap_ = cap;
    mask_ = cap - 1;
    clear();
    return true;
  }

  void clear() noexcept {
    for (uint32_t i = 0; i < cap_; ++i) {
      hi_[i] = 0;
      lo_[i] = 0;
      ocupado_[i] = 0;
    }
    size_ = 0;
  }

  // Devolve true se INSERIU (era novo). False se já estava presente — que é a resposta de I6 —
  // ou se a tabela encheu, e nesse caso `overflow` fica verdadeiro para o chamador tratar como
  // fatal em vez de confundir "cheio" com "duplicado".
  [[nodiscard]] bool insert(uint64_t hi, uint32_t lo, bool& overflow) noexcept {
    overflow = false;
    uint32_t i = slot(hi, lo);
    for (uint32_t passos = 0; passos <= mask_; ++passos) {
      if (ocupado_[i] == 0) {
        // `size_ * 10` era calculado em uint32 e SÓ DEPOIS alargado: transborda a partir de
        // 429 milhões de elementos. Fora de alcance hoje, e exatamente o tipo de limite que
        // ninguém revisita quando a capacidade cresce. O clang-tidy achou os dois.
        if (static_cast<uint64_t>(size_) * 10 >= static_cast<uint64_t>(cap_) * 7) {
          overflow = true;
          return false;
        }
        hi_[i] = hi;
        lo_[i] = lo;
        ocupado_[i] = 1;
        ++size_;
        return true;
      }
      if (hi_[i] == hi && lo_[i] == lo) return false;  // já aplicado
      i = (i + 1) & mask_;
    }
    overflow = true;
    return false;
  }

  [[nodiscard]] bool contains(uint64_t hi, uint32_t lo) const noexcept {
    uint32_t i = slot(hi, lo);
    for (uint32_t passos = 0; passos <= mask_; ++passos) {
      if (ocupado_[i] == 0) return false;
      if (hi_[i] == hi && lo_[i] == lo) return true;
      i = (i + 1) & mask_;
    }
    return false;
  }

  [[nodiscard]] uint32_t size() const noexcept { return size_; }
  [[nodiscard]] uint32_t slots() const noexcept { return cap_; }

  // Percorre as entradas OCUPADAS, em ordem de slot. É o que permite à imagem de recuperação
  // gravar `size()` pares em vez de `slots()` — a tabela é esparsa por construção (fator de carga
  // 0,7), e gravar os buracos é gravar nada com o custo de tudo.
  //
  // A ordem é de slot, não de inserção: quem depende de ordem determinística de ITERAÇÃO está
  // violando D4. Aqui ela serve só para que o conteúdo gravado seja o mesmo em duas execuções da
  // mesma sequência, e isso a ordem de slot garante.
  template <class F>
  void for_each(F&& f) const noexcept {
    for (uint32_t i = 0; i < cap_; ++i) {
      if (ocupado_[i] != 0) f(hi_[i], lo_[i]);
    }
  }

 private:
  [[nodiscard]] uint32_t slot(uint64_t hi, uint32_t lo) const noexcept {
    return static_cast<uint32_t>(mix64(hi ^ (static_cast<uint64_t>(lo) * 0x9E3779B97F4A7C15ULL))) & mask_;
  }
  uint64_t* hi_ = nullptr;
  uint32_t* lo_ = nullptr;
  uint8_t* ocupado_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t mask_ = 0;
  uint32_t size_ = 0;
};

// Conjunto de idempotência com DUAS GERAÇÕES.
//
// O problema: o conjunto de eventos corporativos já aplicados não pode crescer para sempre, e
// remover de uma tabela de endereçamento aberto exige lápide ou reorganização — as duas coisas
// caras e fáceis de errar. A solução é não remover: escreve-se sempre na geração corrente,
// consulta-se as duas, e a cada janela a geração velha é ZERADA e passa a ser a corrente.
//
// A retenção efetiva fica entre uma e duas janelas — com janela de 60 dias, entre 60 e 120. É
// mais do que qualquer reentrega de arquivo da B3 exige, e o custo é um `memset` por rotação.
//
// A rotação é disparada por DATA DE EVENTO (`DayOpened`), nunca por relógio. Se fosse por relógio,
// o replay rotacionaria em pontos diferentes da execução original e o estado divergiria: é I12
// protegendo I6.
class TwoGenSet {
 public:
  [[nodiscard]] bool init(Arena& a, uint32_t max_por_geracao) noexcept {
    return gen_[0].init(a, max_por_geracao) && gen_[1].init(a, max_por_geracao);
  }

  [[nodiscard]] bool contains(uint64_t hi, uint32_t lo) const noexcept {
    return gen_[atual_].contains(hi, lo) || gen_[1 - atual_].contains(hi, lo);
  }

  [[nodiscard]] bool insert(uint64_t hi, uint32_t lo, bool& overflow) noexcept {
    if (contains(hi, lo)) {
      overflow = false;
      return false;
    }
    return gen_[atual_].insert(hi, lo, overflow);
  }

  void maybe_rotate(DateYmd hoje, int32_t janela_dias) noexcept {
    if (rotacao_.empty()) {
      rotacao_ = hoje;
      return;
    }
    if (hoje.day_index() - rotacao_.day_index() < janela_dias) return;
    atual_ = 1 - atual_;
    gen_[atual_].clear();
    rotacao_ = hoje;
  }

  [[nodiscard]] uint32_t size() const noexcept { return gen_[0].size() + gen_[1].size(); }
  [[nodiscard]] DateYmd last_rotation() const noexcept { return rotacao_; }

  // Para a imagem de recuperação: percorre as duas gerações, marcando de qual veio cada par, e
  // reconstrói na mesma geração. Sem a marca, tudo cairia na geração corrente e a próxima rotação
  // esqueceria o que ainda estava dentro da janela.
  template <class F>
  void for_each(F&& f) const noexcept {
    gen_[0].for_each([&](uint64_t hi, uint32_t lo) { f(hi, lo, uint32_t{0}); });
    gen_[1].for_each([&](uint64_t hi, uint32_t lo) { f(hi, lo, uint32_t{1}); });
  }
  [[nodiscard]] bool insert_into(uint32_t geracao, uint64_t hi, uint32_t lo) noexcept {
    bool cheio = false;
    return gen_[geracao & 1U].insert(hi, lo, cheio) && !cheio;
  }
  void set_state(uint32_t atual, DateYmd rotacao) noexcept {
    atual_ = atual & 1U;
    rotacao_ = rotacao;
  }

  struct Counters {
    uint32_t size0, size1, atual;
    DateYmd rotacao;
  };
  [[nodiscard]] Counters counters() const noexcept {
    return Counters{gen_[0].size(), gen_[1].size(), atual_, rotacao_};
  }
  // Não existe `restore(Counters)`: restaurar um TAMANHO sem restaurar as ENTRADAS produziria uma
  // tabela que diz ter n elementos e não encontra nenhum. A imagem de recuperação grava os pares
  // (`for_each`) e os reinsere (`insert_into`); os contadores vêm de brinde.

 private:
  PairIndex gen_[2];
  uint32_t atual_ = 0;
  DateYmd rotacao_{};
};

}  // namespace rv
