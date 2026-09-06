// motor-rv-bench — o harness de medição (ADR-0021).
//
// Três modos, e a distinção entre eles é a razão de o programa existir:
//
//   1. MEDIR (padrão)          — roda as suítes, imprime a tabela, opcionalmente grava o JSON.
//   2. COMPARAR (`--comparar`) — mede e confronta com `bench/baseline.json`. Sai != 0 se houver
//                                regressão acima do limiar. É o gate de PR de ADR-0016.
//   3. FIXAR (`--gravar-baseline`) — grava o próprio `bench/baseline.json`. Recusa fazê-lo quando
//                                a medição não é válida para baseline (build que não é Release,
//                                sanitizer ligado, asserts de invariante ligados, ou qualquer
//                                série instável). Um baseline tirado do preset errado contamina
//                                todos os PRs seguintes, e ninguém descobre olhando o número.
//
// O que ele NÃO faz: decidir sozinho que a máquina é a de referência. `bench/baseline.json` só
// vale para a máquina em que foi tirado (ADR-0022), e o bloco `ambiente` grava qual foi — mas
// quem promove um número a baseline é uma pessoa, com o relatório na mão.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bench/carga.hpp"
#include "bench/comparador.hpp"
#include "bench/harness.hpp"
#include "bench/suites.hpp"

namespace {

void uso() {
  (void)std::puts(
      "uso: motor-rv-bench [opções]\n"
      "\n"
      "  --suites LISTA        base,nucleo,snapshot,wal (padrão: todas)\n"
      "  --repeticoes N        repetições por série (padrão 7)\n"
      "  --aquecimento N       repetições descartadas antes de medir (padrão 2)\n"
      "  --limiar-cv PCT       acima disto a série é descartada e refeita (padrão 5)\n"
      "  --tentativas N        quantas séries refazer antes de desistir (padrão 3)\n"
      "  --rapido              perfil de fumaça para CI: 1 aquecimento, 3 repetições, CV 25%\n"
      "\n"
      "  --dias N              pregões da carga (padrão 1)\n"
      "  --negocios N          negócios por pregão (padrão 20000)\n"
      "  --investidores N      investidores (padrão 2000)\n"
      "  --semente N           semente do simulador (padrão 20260902)\n"
      "  --dados DIR           onde estão instrumentos.csv e calendario-b3-2026.csv\n"
      "  --dir-wal DIR         onde criar o arquivo temporário do WAL (padrão: /tmp)\n"
      "\n"
      "  --out ARQUIVO         grava o JSON da medição (padrão: só a tabela)\n"
      "  --comparar ARQUIVO    confronta com um baseline; sai != 0 se houver regressão\n"
      "  --gravar-baseline ARQ grava o baseline (recusa se a medição não for válida)\n"
      "  --silencioso          sem progresso no stderr\n");
}

[[nodiscard]] bool tem(const std::string& lista, const char* nome) {
  return lista.empty() || lista.find(nome) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  rv::bench::Config cfg{};
  std::string suites;
  std::string dados = MOTOR_RV_DATA_DIR;
  std::string dir_wal = "/tmp";
  std::string saida;
  std::string baseline;
  std::string gravar;
  uint32_t dias = 1, negocios = 20000, investidores = 2000;
  uint64_t semente = 20260902;
  const uint32_t data_inicial = 20260902;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool tem_valor = i + 1 < argc;
    if (a == "--suites" && tem_valor)
      suites = argv[++i];
    else if (a == "--repeticoes" && tem_valor)
      cfg.repeticoes = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--aquecimento" && tem_valor)
      cfg.aquecimento = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--limiar-cv" && tem_valor)
      cfg.limiar_cv_pct = std::strtod(argv[++i], nullptr);
    else if (a == "--tentativas" && tem_valor)
      cfg.tentativas = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--rapido") {
      cfg.aquecimento = 1;
      cfg.repeticoes = 3;
      cfg.limiar_cv_pct = 25.0;
      cfg.tentativas = 1;
    } else if (a == "--dias" && tem_valor)
      dias = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--negocios" && tem_valor)
      negocios = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--investidores" && tem_valor)
      investidores = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    else if (a == "--semente" && tem_valor)
      semente = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--dados" && tem_valor)
      dados = argv[++i];
    else if (a == "--dir-wal" && tem_valor)
      dir_wal = argv[++i];
    else if (a == "--out" && tem_valor)
      saida = argv[++i];
    else if (a == "--comparar" && tem_valor)
      baseline = argv[++i];
    else if (a == "--gravar-baseline" && tem_valor)
      gravar = argv[++i];
    else if (a == "--silencioso")
      cfg.verboso = false;
    else {
      uso();
      return (a == "--help" || a == "-h") ? 0 : 2;
    }
  }

  if (cfg.repeticoes < 2 || cfg.tentativas < 1) {
    (void)std::fprintf(stderr,
                       "--repeticoes >= 2 e --tentativas >= 1: sem isso não há dispersão "
                       "para julgar a estabilidade da série.\n");
    return 2;
  }

  const rv::bench::Ambiente amb = rv::bench::coleta_ambiente(dir_wal);
  (void)std::printf("== motor-rv-bench ==\n");
  (void)std::printf("  commit    : %s\n", amb.commit.c_str());
  (void)std::printf("  máquina   : %s\n", amb.maquina.c_str());
  (void)std::printf("  kernel    : %s\n", amb.kernel.c_str());
  (void)std::printf("  flags     : %s\n", amb.flags.c_str());
  (void)std::printf("  wal       : %s\n", amb.dispositivo_wal.c_str());
  if (!amb.valido_para_baseline()) {
    // O aviso é gritado de propósito. `docs/ambiente.md` e o preset `debug` já dizem "nunca use
    // para medir nada"; um número impresso sem aviso é lido como se valesse.
    (void)std::printf(
        "\n  *** ESTA MEDIÇÃO NÃO VALE COMO BASELINE: %s ***\n"
        "  Use o preset `release` (cmake --preset release). Ver ADR-0022.\n",
        amb.por_que_invalido().c_str());
  }

  rv::bench::Runner runner{cfg};

  rv::bench::Carga carga;
  const bool precisa_carga = tem(suites, "nucleo") || tem(suites, "snapshot");
  if (precisa_carga) {
    carga = rv::bench::gera_carga(dados, dias, negocios, investidores, semente, data_inicial);
    if (!carga.ok) {
      (void)std::fprintf(stderr, "motor-rv-bench: %s\n", carga.erro.c_str());
      return 1;
    }
    (void)std::printf(
        "  carga     : %zu eventos (%u pregão(ões), %u negócios/dia, %u investidores,"
        " semente %llu)\n",
        carga.tamanho(), dias, negocios, investidores, static_cast<unsigned long long>(semente));
  }
  (void)std::printf("  harness   : aquecimento=%u repetições=%u limiar_cv=%.1f%% tentativas=%u\n\n",
                    cfg.aquecimento, cfg.repeticoes, cfg.limiar_cv_pct, cfg.tentativas);

  if (tem(suites, "base")) rv::bench::registra_base(runner);
  if (tem(suites, "nucleo")) rv::bench::registra_nucleo(runner, carga);
  if (tem(suites, "snapshot")) rv::bench::registra_snapshot(runner, carga);
  if (tem(suites, "wal")) rv::bench::registra_wal(runner, dir_wal);

  rv::bench::imprime_tabela(runner.series());

  const std::string status =
      amb.valido_para_baseline()
          ? std::string("medição do harness (ADR-0021)")
          : std::string("medição INVÁLIDA para baseline: ") + amb.por_que_invalido();

  if (!saida.empty()) {
    std::string erro;
    if (!rv::bench::escreve_json(saida, amb, cfg, carga.descricao(), runner.series(), status,
                                 erro)) {
      (void)std::fprintf(stderr, "motor-rv-bench: %s\n", erro.c_str());
      return 1;
    }
    (void)std::printf("medição gravada em %s\n", saida.c_str());
  }

  int codigo = 0;

  if (!baseline.empty()) {
    rv::bench::DocumentoJson doc;
    std::string erro;
    if (!rv::bench::le_json(baseline, doc, erro)) {
      (void)std::fprintf(stderr, "motor-rv-bench: %s\n", erro.c_str());
      return 1;
    }
    // O limiar é do ARQUIVO, não do harness: quem fixou o baseline também fixou quanto de
    // variação ele tolera, e mudar isso é mudar o arquivo — não passar uma opção na linha de
    // comando na hora de rodar o gate.
    double limiar = 5.0;
    const auto it_limiar = doc.find("limiar_regressao_pct");
    if (it_limiar != doc.end() && it_limiar->second.tipo == rv::bench::ValorJson::Tipo::Numero) {
      limiar = it_limiar->second.numero;
    }
    const rv::bench::Veredito v =
        rv::bench::compara(doc, runner.series(), limiar, carga.descricao());
    rv::bench::imprime_veredito(v, limiar);
    if (v.houve_regressao) codigo = 3;
    // Carga incompatível não é regressão, mas também não é aprovação: o gate não conferiu nada.
    if (v.carga_incompativel) codigo = 4;
  }

  if (!gravar.empty()) {
    // As duas recusas existem pelo mesmo motivo: um baseline errado não dá erro, dá aprovação
    // errada em todo PR seguinte.
    if (!amb.valido_para_baseline()) {
      (void)std::fprintf(stderr,
                         "motor-rv-bench: recuso gravar baseline — %s.\n"
                         "Rode com o preset `release`.\n",
                         amb.por_que_invalido().c_str());
      return 2;
    }
    bool alguma_instavel = false;
    for (const rv::bench::Serie& s : runner.series()) {
      if (s.medida && !s.estavel) {
        (void)std::fprintf(stderr, "motor-rv-bench: série instável: %s (CV %.2f%%)\n",
                           s.nome.c_str(), s.cv_pct);
        alguma_instavel = true;
      }
    }
    if (alguma_instavel) {
      (void)std::fprintf(stderr,
                         "motor-rv-bench: recuso gravar baseline com série instável. Reduza o "
                         "ruído da máquina ou aumente --repeticoes.\n");
      return 2;
    }
    std::string erro;
    if (!rv::bench::escreve_json(gravar, amb, cfg, carga.descricao(), runner.series(),
                                 "baseline fixado pelo harness", erro)) {
      (void)std::fprintf(stderr, "motor-rv-bench: %s\n", erro.c_str());
      return 1;
    }
    (void)std::printf("baseline gravado em %s\n", gravar.c_str());
  }

  return codigo;
}
