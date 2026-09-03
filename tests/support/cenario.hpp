#pragma once
// Um gerador de sequências de eventos, determinístico por construção.
//
// Nada de `std::random_device`, nada de relógio: um LCG com semente fixa. É a mesma disciplina
// que o `apply` obedece (D2), e por uma razão prática — um teste de replay que usasse fonte de
// aleatoriedade real falharia de vez em quando e ninguém saberia reproduzir.

#include <vector>

#include <cstring>

#include "codec/events.hpp"
#include "core/partition.hpp"
#include "engine_fixture.hpp"  // construtores de evento e as constantes de documento

namespace rv::testing {

inline constexpr uint32_t kD0 = 20260908, kD1 = 20260909, kD2 = 20260910;
inline constexpr uint32_t kD0b = 20260910, kD1b = 20260911, kD2b = 20260914;

class Lcg {
 public:
  explicit constexpr Lcg(uint64_t semente) noexcept : s_(semente) {}
  constexpr uint64_t next() noexcept {
    s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return s_ >> 16;
  }
  constexpr uint32_t below(uint32_t n) noexcept { return static_cast<uint32_t>(next() % n); }

 private:
  uint64_t s_;
};

// Um evento pronto para entrar no ring: template, tamanho e bytes.
struct Evento {
  uint16_t tmpl;
  uint16_t len;
  std::byte bytes[core::kMaxIngressPayload];
};

template <class T>
inline Evento faz(const T& m) {
  Evento e{};
  e.tmpl = T::kTemplateId;
  e.len = T::kBlockLength;
  std::memcpy(e.bytes, &m, T::kBlockLength);
  return e;
}

// Uma sessão plausível: abre o dia, cadastra instrumentos, negocia, aloca, compensa, liquida,
// aplica eventos corporativos e proventos, vira o dia, fecha o EOD.
//
// A mistura inclui, DE PROPÓSITO, eventos que serão rejeitados (venda descoberta sem flag, data
// fora da janela, provento com aritmética errada). Um teste de replay que só usasse eventos
// aceitos não provaria a parte mais fácil de quebrar: o replay tem de rejeitar os MESMOS eventos,
// pelo MESMO motivo, ou o estado diverge.
inline std::vector<Evento> gera_sessao(uint64_t semente, uint32_t n_negocios) {
  Lcg r{semente};
  std::vector<Evento> v;
  v.push_back(faz(dia(kD0, kD1, kD2)));

  constexpr uint32_t kNInstr = 5;
  for (uint32_t i = 1; i <= kNInstr; ++i) {
    v.push_back(faz(cadastro(i, "TICK", i == kXpto11 ? 1000 : 1,
                             static_cast<int64_t>(1'000'000'000 + i * 137'000'000), kD0)));
  }

  const uint64_t docs[] = {kCpfA, kCpfB, 99988877766ULL, 12345678000199ULL};
  uint64_t id = 1;
  for (uint32_t i = 0; i < n_negocios; ++i) {
    const uint64_t doc = docs[r.below(4)];
    const uint32_t inst = 1 + r.below(kNInstr);
    const bool compra = r.below(100) < 60;
    const int64_t qty = Qty::from_units(static_cast<int64_t>(1 + r.below(500))).raw();
    const int64_t preco = 500'000'000 + static_cast<int64_t>(r.below(4'000'000'000u));
    const int64_t custos = static_cast<int64_t>(r.below(100'000));
    // Uma em vinte tem data fora da janela: será rejeitada, e o replay tem de rejeitá-la igual.
    const uint32_t data = (r.below(20) == 0) ? 20261231 : kD2;
    v.push_back(faz(negocio(id, doc, inst, compra ? codec::Side::Buy : codec::Side::Sell, qty,
                            preco, custos, data)));
    v.push_back(faz(alocacao(id, id, doc, doc, inst, compra ? codec::Side::Buy : codec::Side::Sell,
                             qty, 0, data)));
    ++id;
  }

  // Liquidação parcial: metade dos papéis de cada conta, com custo declarado.
  for (uint64_t doc : docs) {
    for (uint32_t inst = 1; inst <= kNInstr; ++inst) {
      v.push_back(faz(liquidacao(1000 + inst, doc, inst, codec::Side::Buy,
                                 Qty::from_units(10).raw(), -1'000'000, 1'000'000, kD2,
                                 codec::SettleOutcome::Settled)));
    }
  }

  // Eventos corporativos, incluindo uma duplicata (I6) e um provento malformado (rejeitado).
  for (uint64_t doc : docs) {
    auto split = corporativo(9000 + (doc & 0xFF), doc, 1, codec::ActionType::Split, 2, 1, kD1);
    v.push_back(faz(split));
    v.push_back(faz(split));  // duplicata: tem de ser recusada nas duas execuções
    auto p = provento(8000 + (doc & 0xFF), doc, 2, codec::IncomeKind::Dividend,
                      Qty::from_units(10).raw(), 1000, 10'000, 0, kD1,
                      codec::IncomeStage::Accrued);
    v.push_back(faz(p));
    auto ruim = p;
    ruim.action_id += 500;
    ruim.net_amount = 1;  // bruto − retido ≠ líquido: rejeitado
    v.push_back(faz(ruim));
  }

  v.push_back(faz(dia(kD0b, kD1b, kD2b, kD0)));
  return v;
}

}  // namespace rv::testing
