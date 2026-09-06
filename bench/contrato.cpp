#include "bench/contrato.hpp"

namespace rv::bench {

const Serie* serie_de(const std::vector<Serie>& series, const char* nome) noexcept {
  if (nome == nullptr) return nullptr;
  for (const Serie& s : series) {
    if (s.nome == nome) return &s;
  }
  return nullptr;
}

std::vector<std::string> contrato_quebrado(const std::vector<Serie>& series) {
  std::vector<std::string> faltando;
  for (const Metrica& m : kEsquemaMetricas) {
    if (m.serie == nullptr) continue;
    // "A suíte rodou" é lido da própria lista de séries, e não de `--suites`: quem escolhe as
    // suítes é o `main`, mas quem sabe o que de fato foi registrado é esta lista. Uma suíte que
    // desiste no meio (a partição não coube na arena) registra a série com `pula` e continua
    // presente aqui — é uma medição ausente com motivo, não um contrato quebrado.
    bool grupo_rodou = false;
    bool achou = false;
    for (const Serie& s : series) {
      if (s.grupo == m.grupo) grupo_rodou = true;
      if (s.nome == m.serie) achou = true;
    }
    if (grupo_rodou && !achou) {
      faltando.emplace_back(std::string(m.serie) + " (alimenta " + m.chave + ")");
    }
  }
  return faltando;
}

}  // namespace rv::bench
