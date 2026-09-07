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
  // Métricas contratuais que ESTA execução mediu mal (pulada ou instável) e por isso não entraram
  // na comparação. Elas saíam por um `continue` silencioso: o gate imprimia uma tabela sem a linha
  // que importa e saía 0. Não comparar não é aprovar.
  std::vector<std::string> nao_comparadas;
  bool houve_regressao = false;
  bool baseline_vazio = true;
  // Carga diferente da do baseline: NÃO se compara. Ver DescricaoCarga em harness.hpp.
  bool carga_incompativel = false;
  std::string motivo_carga;
  // Os campos da carga que o baseline não declara — e que por isso NÃO foram conferidos. Existe
  // porque "as cargas batem" e "não deu para saber se batem" são fatos diferentes, e antes disto
  // os dois produziam exatamente a mesma saída.
  std::vector<std::string> carga_nao_conferida;
  bool execucao_sem_carga = false;

  // Comparou números sem ter podido conferir a carga deles. É o caso que o portão de carga
  // deixava passar como aprovação; agora ele tem nome, sai no veredito e vira código 5.
  [[nodiscard]] bool comparou_sem_conferir_carga() const noexcept {
    return !comparacoes.empty() && (execucao_sem_carga || !carga_nao_conferida.empty());
  }
};

// Compara as séries medidas contra `bench/baseline.json`. Métricas nulas no baseline não são
// falha: são "ainda não fixado", e dizê-lo é diferente de aprovar em silêncio.
//
// Se a carga do baseline for diferente da carga desta execução, nada é comparado: o veredito volta
// com `carga_incompativel` e o motivo. Comparar assim mediria a diferença entre dois experimentos,
// não entre duas versões do motor.
//
// Se o baseline NÃO declarar a carga, a comparação acontece — um baseline antigo ainda tem números
// úteis — mas o veredito volta dizendo quais campos ficaram sem conferência. Um portão que não
// conferiu nada e um portão que aprovou têm de ser distinguíveis por quem lê a saída.
[[nodiscard]] Veredito compara(const DocumentoJson& baseline, const std::vector<Serie>& series,
                               double limiar_pct, const DescricaoCarga& carga);

// `arquivo` é o baseline confrontado, e não a constante "bench/baseline.json": o gate também é
// rodado contra baselines sintéticos (o workflow o faz), e um cabeçalho que nomeia o arquivo
// errado é a mesma classe de erro que o resto deste arquivo existe para evitar.
// O código de saída que este veredito produz, com a precedência do projeto: 3 (regressão) antes de
// 4 (carga incompatível), antes de 6 (métrica contratual não comparada), antes de 5 (carga não
// conferida). Uma função só porque `main` e `imprime_veredito` já discordaram: o texto anunciava
// "é o que o código 5 diz" num caso que saía 3.
[[nodiscard]] int codigo_de(const Veredito& v) noexcept;

void imprime_veredito(const Veredito& v, double limiar_pct, const std::string& arquivo);

}  // namespace rv::bench
