#include "bench/contrato.hpp"

#include <algorithm>
#include <string_view>

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

std::vector<std::pair<std::string, uint64_t>> campos_da_carga(const DescricaoCarga& carga) {
  return {
      {"dias", carga.dias},
      {"negocios_por_dia", carga.negocios_por_dia},
      {"investidores", carga.investidores},
      {"particoes", carga.particoes},
      {"semente", carga.semente},
  };
}

namespace {

// Os subcampos de uma métrica de quantis ("p50,p99") como caminhos completos do documento.
[[nodiscard]] std::vector<std::string> caminhos_de(const Metrica& m) {
  const std::string base = std::string("metricas.") + m.chave;
  if (m.forma == FormaValor::Escalar) return {base};
  if (m.forma != FormaValor::Objeto) return {};  // Mapa: objeto vazio não deixa caminho nenhum
  std::vector<std::string> saida;
  std::string campo;
  for (const char* p = m.subcampos;; ++p) {
    if (*p != ',' && *p != '\0') {
      campo += *p;
      continue;
    }
    if (!campo.empty()) {
      std::string caminho = base;
      caminho += '.';
      caminho += campo;
      saida.push_back(caminho);
      campo.clear();
    }
    if (*p == '\0') break;
  }
  return saida;
}

// A chave do documento pertence a alguma métrica do esquema? Ela vem achatada ("metricas.a.b.p50")
// e a própria chave da métrica tem pontos, então a comparação é por igualdade ou por prefixo
// seguido de ponto — não por split.
[[nodiscard]] bool de_alguma_metrica(const std::string& caminho) {
  constexpr std::string_view kPrefixo = "metricas.";
  const std::string resto = caminho.substr(kPrefixo.size());
  return std::ranges::any_of(kEsquemaMetricas, [&resto](const Metrica& m) {
    const std::string chave = m.chave;
    return resto == chave || resto.starts_with(chave + ".");
  });
}

}  // namespace

std::vector<std::string> divergencias_de_esquema(const std::vector<std::string>& caminhos) {
  std::vector<std::string> ruins;

  // 1. o que o esquema exige e o arquivo não tem.
  for (const Metrica& m : kEsquemaMetricas) {
    for (const std::string& esperado : caminhos_de(m)) {
      if (std::ranges::find(caminhos, esperado) == caminhos.end()) {
        ruins.push_back("falta no arquivo: " + esperado);
      }
    }
  }

  // 2. o que o arquivo traz e o esquema não conhece — o caso do rename feito só no arquivo.
  for (const std::string& k : caminhos) {
    if (std::string_view(k).starts_with("metricas.") && !de_alguma_metrica(k)) {
      ruins.push_back("o esquema não conhece: " + k);
    }
  }
  return ruins;
}

}  // namespace rv::bench
