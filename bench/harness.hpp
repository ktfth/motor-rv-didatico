#pragma once
// O harness de medição do motor-rv (ADR-0021).
//
// ---------------------------------------------------------------------------------------------
// O QUE ELE É, E POR QUE NÃO É UM FRAMEWORK DE MICROBENCHMARK
//
// Google Benchmark não existe na máquina de referência (docs/ambiente.md), mas a ausência não é
// a razão principal. As métricas obrigatórias deste projeto não são "tempo médio de uma função":
// são DISTRIBUIÇÕES ponta a ponta (append→durável P50/P99/P999), tamanho de grupo, duração de
// snapshot, eventos por segundo por core. Um framework que reporta média e desvio de uma chamada
// mede a coisa errada com muita cerimônia.
//
// Quatro propriedades, e cada uma existe por um erro conhecido:
//
//   1. AQUECIMENTO EXPLÍCITO. As primeiras repetições rodam e são jogadas fora. Sem isso, mede-se
//      falta de cache e o primeiro toque de página da arena — que o motor de produção paga uma vez
//      no warm-up e nunca mais.
//   2. REPETIÇÕES E DESCARTE DE SÉRIE. Uma série cujo coeficiente de variação passa do limiar é
//      DESCARTADA e refeita. Persistindo a instabilidade, a série é publicada como `estavel:false`
//      e a métrica correspondente sai `null` — nunca vira número de baseline. Uma medição instável
//      publicada como se fosse estável é pior que medição nenhuma: ela autoriza uma otimização que
//      não se sabe se ganhou.
//   3. SEM ALOCAÇÃO DENTRO DA REGIÃO MEDIDA. O corpo devolve `Amostra{operacoes, ns}` e decide o
//      que está dentro do cronômetro; a montagem do cenário fica fora. Os histogramas são
//      `rv::Histogram` — 4 KiB fixos, zero alocação em `record`.
//   4. SAÍDA NO FORMATO DE `bench/baseline.json`, com o bloco `ambiente` preenchido pelo PROGRAMA.
//      O arquivo passa a ser saída de programa, e não número colado do terminal — o que remove a
//      classe inteira de erro de "colei do build errado".
//
// ---------------------------------------------------------------------------------------------
// SOBRE `double` AQUI E A REGRA §2 DO CODING_RULES
//
// CODING_RULES §2 proíbe `double` em DINHEIRO, QUANTIDADE e PREÇO. Estatística de tempo não é
// nenhuma das três: mediana e desvio padrão de nanossegundos são grandezas físicas, não valores
// de ledger, e arredondá-las não deve dinheiro a ninguém. O que o harness NUNCA faz é converter
// um `Fixed` para `double` — nenhum número do motor passa por aqui.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "base/metrics.hpp"

namespace rv::bench {

// Relógio de medição: `CLOCK_MONOTONIC`. Não é o relógio do motor — o `apply` não tem relógio
// nenhum (D2), e é justamente por isso que medi-lo de fora é legítimo.
[[nodiscard]] uint64_t agora_ns() noexcept;

// O que uma repetição devolve. `operacoes` é a unidade de normalização: eventos, mensagens,
// bytes, ou 1 quando a própria duração é a métrica (o caso do snapshot).
struct Amostra {
  uint64_t operacoes = 0;
  uint64_t ns = 0;
};

// Qual direção é melhora. Não é enfeite: é o que o comparador usa para decidir se uma variação de
// −8 % é regressão (vazão) ou ganho (latência). Deixar isso implícito no nome da métrica é como o
// comparador erra o sinal.
enum class Direcao : uint8_t { MaiorMelhor, MenorMelhor };

// COMO a amostra vira número. A forma decide as duas coisas que ninguém deve ter de lembrar de
// combinar à mão: a normalização (`ns` e `operacoes` viram um valor só) e a direção da melhora.
//
// Elas andam juntas de propósito. Enquanto eram dois parâmetros independentes, existia o par
// inconsistente — uma duração declarada como "maior é melhor" — e o comparador o aceitaria sem
// piscar, invertendo o sinal de toda regressão daquela métrica.
enum class Forma : uint8_t {
  Taxa,       // operações por segundo; maior é melhor
  Vazao,      // `operacoes` são BYTES, o valor sai em MiB/s; maior é melhor
  DuracaoNs,  // nanossegundos por operação; menor é melhor
  DuracaoUs,  // microssegundos por operação; menor é melhor
  DuracaoMs,  // milissegundos por operação; menor é melhor
  Tamanho,    // o valor É `operacoes` (bytes); o tempo é ignorado; menor é melhor
};

[[nodiscard]] constexpr Direcao direcao_de(Forma f) noexcept {
  return f == Forma::Taxa || f == Forma::Vazao ? Direcao::MaiorMelhor : Direcao::MenorMelhor;
}

// A unidade que o JSON e a tabela publicam para cada forma. Uma fonte só: a unidade impressa não
// pode divergir da conta que produziu o número.
[[nodiscard]] const char* unidade_de(Forma f) noexcept;

struct Serie {
  std::string grupo;
  std::string nome;     // chave estável; é o que aparece no JSON e no comparador
  std::string unidade;  // "eventos/s", "ns", "MiB/s", "ms" — derivada da forma
  std::string nota;     // por que foi pulada, ou o que o número NÃO significa
  Forma forma = Forma::Taxa;
  Direcao direcao = Direcao::MaiorMelhor;

  bool medida = false;   // false = pulada (recurso ausente); `nota` diz por quê
  bool estavel = false;  // false = CV acima do limiar depois de todas as tentativas

  std::vector<double> amostras;  // uma por repetição aceita, já normalizada por operação
  double mediana = 0.0;
  double media = 0.0;
  double cv_pct = 0.0;
  double minimo = 0.0;
  double maximo = 0.0;
  uint32_t series_descartadas = 0;
  uint64_t operacoes_por_repeticao = 0;

  // Quantis, quando a medição registrou latência POR OPERAÇÃO num histograma.
  bool tem_quantis = false;
  uint64_t p50 = 0, p99 = 0, p999 = 0, pmax = 0;
};

// A carga que produziu os números, gravada JUNTO deles.
//
// Sem isto o arquivo mente por omissão, e a medição desta mesma sessão prova como: um pregão dá
// 7,3 M eventos/s e três pregões dão 5,3 M — o estado cresce, a lista de negócios por conta fica
// mais longa, a liquidação percorre mais. São dois números certos da MESMA métrica, e compará-los
// entre si não significa nada. Por isso o comparador RECUSA baseline com carga diferente em vez
// de reportar uma regressão de 28 % que não existe.
struct DescricaoCarga {
  uint32_t dias = 0;
  uint32_t negocios_por_dia = 0;
  uint32_t investidores = 0;
  uint32_t particoes = 0;
  uint64_t semente = 0;
  uint64_t eventos = 0;
  [[nodiscard]] bool vazia() const noexcept { return eventos == 0; }
};

struct Config {
  uint32_t aquecimento = 2;
  uint32_t repeticoes = 7;
  double limiar_cv_pct = 5.0;
  uint32_t tentativas = 3;  // quantas séries inteiras refazer antes de desistir da estabilidade
  bool verboso = true;
};

// O bloco `ambiente` de bench/baseline.json. Preenchido pelo programa: nenhum campo é digitado.
struct Ambiente {
  std::string commit;
  std::string maquina;
  std::string kernel;
  std::string flags;
  std::string dispositivo_wal;
  // Derivados que decidem se a medição pode virar baseline.
  bool release = false;
  bool sem_sanitizer = false;
  bool asserts_desligados = false;
  [[nodiscard]] bool valido_para_baseline() const noexcept {
    return release && sem_sanitizer && asserts_desligados;
  }
  [[nodiscard]] std::string por_que_invalido() const;
};

[[nodiscard]] Ambiente coleta_ambiente(const std::string& caminho_wal);

class Runner {
 public:
  explicit Runner(Config cfg) noexcept : cfg_(cfg) {}

  // Vazão ou duração: o corpo devolve quantas operações fez e quanto tempo elas levaram.
  const Serie& medir(const char* grupo, const char* nome, Forma forma,
                     const std::function<Amostra()>& corpo);

  // Distribuição: o corpo registra a latência de CADA operação no histograma que recebe. O
  // histograma acumula apenas as repetições da série aceita — uma série descartada não deixa
  // resíduo nos quantis, que é o erro clássico de quem soma tudo e depois filtra.
  const Serie& medir_latencia(const char* grupo, const char* nome, Forma forma,
                              const std::function<Amostra(Histogram&)>& corpo);

  // Registra uma série que NÃO foi medida, com o motivo. Pular em silêncio é como um relatório
  // passa a mentir por omissão.
  const Serie& pula(const char* grupo, const char* nome, Forma forma, const std::string& motivo);

  // Acrescenta à `nota` da última série registrada — o regime em que ela foi medida (com ou sem
  // `O_DIRECT`, com ou sem `DEFER_TASKRUN`). A nota viaja no JSON, junto do número: quem lê só o
  // arquivo tem de saber em que condições ele foi tirado.
  void anota(const std::string& texto);

  [[nodiscard]] const std::vector<Serie>& series() const noexcept { return series_; }
  [[nodiscard]] const Config& config() const noexcept { return cfg_; }

 private:
  Serie roda_(const char* grupo, const char* nome, Forma forma,
              const std::function<Amostra(Histogram*)>& corpo, Histogram* hist);
  const Serie& guarda_(Serie s);

  Config cfg_;
  std::vector<Serie> series_;
};

// ---------------------------------------------------------------------------- estatística
[[nodiscard]] double mediana_de(std::vector<double> v);  // por valor: ordena a cópia
[[nodiscard]] double media_de(const std::vector<double>& v) noexcept;
[[nodiscard]] double cv_pct_de(const std::vector<double>& v) noexcept;

// ---------------------------------------------------------------------------- saída
// Escreve o documento de medição (o mesmo esquema de bench/baseline.json, mais `series`).
// `caminho` vazio = stdout.
[[nodiscard]] bool escreve_json(const std::string& caminho, const Ambiente& amb, const Config& cfg,
                                const DescricaoCarga& carga, const std::vector<Serie>& series,
                                const std::string& status, std::string& erro);

// Tabela legível para o terminal. O JSON é para máquina; isto é para quem está olhando.
void imprime_tabela(const std::vector<Serie>& series);

}  // namespace rv::bench
