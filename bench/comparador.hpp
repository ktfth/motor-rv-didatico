#pragma once
// O comparador de baseline: aplica o limiar de `bench/baseline.json` e SAI COM CÓDIGO DIFERENTE
// DE ZERO quando há regressão (ADR-0021).
//
// É a metade do ADR que faz o baseline valer alguma coisa. Um arquivo de números que ninguém
// confere é documentação; um arquivo que reprova um PR é gate.
//
// O leitor de JSON daqui NÃO é um parser de propósito geral, e não pode virar um. Ele lê um
// arquivo que este mesmo programa escreveu, num esquema fechado, sem entrada externa. O parser de
// JSON do projeto — o que enfrenta requisição de terceiro — é o da borda (ADR-0018), com limites
// de tamanho e harness de fuzz. Reusar este ali seria expor a superfície errada.

#include <map>
#include <string>
#include <vector>

#include "bench/harness.hpp"

namespace rv::bench {

// Um valor do documento, endereçado por caminho ("metricas.snapshot.duracao_ms"). Só o que o
// esquema tem: número, nulo, texto.
struct ValorJson {
  enum class Tipo : uint8_t { Nulo, Numero, Texto } tipo = Tipo::Nulo;
  double numero = 0.0;
  std::string texto;
};

using DocumentoJson = std::map<std::string, ValorJson>;

[[nodiscard]] bool le_json(const std::string& caminho, DocumentoJson& out, std::string& erro);

struct Comparacao {
  std::string chave;
  double baseline = 0.0;
  double medido = 0.0;
  double variacao_pct = 0.0;
  Direcao direcao = Direcao::MaiorMelhor;
  bool regressao = false;
};

struct Veredito {
  std::vector<Comparacao> comparacoes;
  std::vector<std::string> sem_baseline;  // métricas que o baseline ainda não fixou
  bool houve_regressao = false;
  bool baseline_vazio = true;
  // Carga diferente da do baseline: NÃO se compara. Ver DescricaoCarga em harness.hpp.
  bool carga_incompativel = false;
  std::string motivo_carga;
};

// Compara as séries medidas contra `bench/baseline.json`. Métricas nulas no baseline não são
// falha: são "ainda não fixado", e dizê-lo é diferente de aprovar em silêncio.
//
// Se a carga do baseline for diferente da carga desta execução, nada é comparado: o veredito volta
// com `carga_incompativel` e o motivo. Comparar assim mediria a diferença entre dois experimentos,
// não entre duas versões do motor.
[[nodiscard]] Veredito compara(const DocumentoJson& baseline, const std::vector<Serie>& series,
                               double limiar_pct, const DescricaoCarga& carga);

void imprime_veredito(const Veredito& v, double limiar_pct);

}  // namespace rv::bench
