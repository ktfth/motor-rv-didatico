// A saída do harness: o MESMO esquema de bench/baseline.json, mais o detalhe das séries.
//
// ---------------------------------------------------------------------------------------------
// O BLOCO `metricas` É GERADO DA TABELA, NÃO DIGITADO
//
// Ele já foi prosa: sete chaves escritas à mão em `fputs`, com os `null` literais dentro, e a
// tabela de correspondência série → chave logo acima sendo ignorada pelo texto ao lado dela.
// Acrescentar uma métrica ao esquema custava editar três lugares e lembrar de todos; esquecer um
// não dava erro nenhum — dava uma chave a menos no arquivo que o comparador procura por chave.
//
// Agora a única descrição do esquema é `kEsquemaMetricas`, em bench/contrato.hpp, e este arquivo
// a percorre. Uma linha na tabela é uma métrica no JSON, no `metricas_ausentes` e no bloco
// `contrato` que o relatório em Python consome.
//
// ---------------------------------------------------------------------------------------------
// POR QUE `metricas_ausentes` EXISTE
//
// `bench/baseline.json` tem sete métricas obrigatórias e o motor hoje só consegue medir duas.
// Havia dois jeitos de escrever isso: deixar as outras cinco em `null` e ninguém saber por quê,
// ou explicar cada uma. O primeiro é como um baseline vira folclore — daqui a três meses alguém
// pergunta "por que a latência do WAL é nula?" e a resposta depende da memória de alguém.
//
// Então cada métrica não medida sai com o MOTIVO ao lado, e o motivo é sempre da mesma forma:
// que peça de código ainda não existe. Quando a peça existir, a linha some sozinha — porque
// `contrato.hpp` passa a nomear a série que a preenche e o motivo sai da tabela.

#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "bench/contrato.hpp"
#include "bench/harness.hpp"

namespace rv::bench {
namespace {

void escapa(std::FILE* f, const std::string& s) {
  (void)std::fputc('"', f);
  for (const char c : s) {
    switch (c) {
      case '"':
        (void)std::fputs("\\\"", f);
        break;
      case '\\':
        (void)std::fputs("\\\\", f);
        break;
      case '\n':
        (void)std::fputs("\\n", f);
        break;
      case '\r':
        (void)std::fputs("\\r", f);
        break;
      case '\t':
        (void)std::fputs("\\t", f);
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          (void)std::fprintf(f, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
          (void)std::fputc(c, f);
        }
    }
  }
  (void)std::fputc('"', f);
}

// Números do JSON com casas fixas. `%.6g` produziria `1e+06`, que é JSON válido e um convite a
// erro de leitura humana no diff de um baseline.
void numero(std::FILE* f, double v) {
  if (!std::isfinite(v)) {
    (void)std::fputs("null", f);
    return;
  }
  (void)std::fprintf(f, "%.4f", v);
}

// Os quantis de uma métrica que ainda não é medida: `{ "p50": null, "p99": null }`. Os nomes vêm
// da tabela, separados por vírgula, porque a alternativa era outra estrutura para descrever dois
// literais — e a lista está ao lado do motivo pelo qual eles são nulos.
void objeto_de_nulos(std::FILE* f, const char* subcampos) {
  (void)std::fputs("{", f);
  std::string campo;
  bool primeiro = true;
  const auto emite = [&] {
    if (campo.empty()) return;
    (void)std::fputs(primeiro ? " " : ", ", f);
    primeiro = false;
    escapa(f, campo);
    (void)std::fputs(": null", f);
    campo.clear();
  };
  for (const char* p = subcampos; *p != '\0'; ++p) {
    if (*p == ',') {
      emite();
    } else {
      campo += *p;
    }
  }
  emite();
  (void)std::fputs(" }", f);
}

// Uma série só vira número de baseline se foi medida E ficou estável. É a regra de ADR-0021
// aplicada no ponto onde ela vale: na escrita do arquivo, não na cabeça de quem lê o terminal.
[[nodiscard]] bool publicavel(const Serie* s) noexcept {
  return s != nullptr && s->medida && s->estavel;
}

// O bloco `contrato`: as métricas obrigatórias com o rótulo em português corrente, e os campos
// que definem a carga. `scripts/relatorio-bench.py` DERIVA daqui as séries que tem de comparar,
// os rótulos do resumo e os campos de carga que confere — em vez de repetir as duas listas em
// Python, que foi como a terceira e a quarta cópias da mesma verdade nasceram.
void escreve_contrato(std::FILE* f, const DescricaoCarga& carga) {
  (void)std::fputs("  \"contrato\": {\n    \"metricas\": [\n", f);
  bool primeiro = true;
  for (const Metrica& m : kEsquemaMetricas) {
    if (m.serie == nullptr) continue;
    if (!primeiro) (void)std::fputs(",\n", f);
    primeiro = false;
    (void)std::fputs("      { \"chave\": ", f);
    escapa(f, m.chave);
    (void)std::fputs(", \"serie\": ", f);
    escapa(f, m.serie);
    (void)std::fputs(", \"rotulo\": ", f);
    escapa(f, m.rotulo);
    (void)std::fputs(" }", f);
  }
  // As séries que REPROVAM um PR. Não é a mesma lista das métricas de baseline, e a diferença é o
  // ponto: enquanto era, só duas séries podiam reprovar e uma piora de 195 % na restauração passava
  // como informativa. Ver bench/contrato.hpp.
  (void)std::fputs("\n    ],\n    \"bloqueantes\": [\n", f);
  primeiro = true;
  for (const SerieBloqueante& b : kSeriesBloqueantes) {
    if (!primeiro) (void)std::fputs(",\n", f);
    primeiro = false;
    (void)std::fputs("      { \"serie\": ", f);
    escapa(f, b.serie);
    (void)std::fputs(", \"rotulo\": ", f);
    escapa(f, b.rotulo);
    (void)std::fputs(" }", f);
  }
  (void)std::fputs("\n    ],\n    \"campos_carga\": [", f);
  primeiro = true;
  for (const auto& [nome, valor] : campos_da_carga(carga)) {
    (void)valor;  // aqui interessa o NOME do campo; o valor já saiu no bloco `carga`
    if (!primeiro) (void)std::fputs(", ", f);
    primeiro = false;
    escapa(f, nome);
  }
  (void)std::fputs("]\n  },\n", f);
}

// O bloco `metricas`, na ordem de `kEsquemaMetricas` — que é a ordem de bench/baseline.json,
// inclusive as nulas: o comparador casa por chave, e um arquivo de medição com esquema diferente
// do baseline não compara nada.
void escreve_metricas(std::FILE* f, const std::vector<Serie>& series) {
  (void)std::fputs("  \"metricas\": {\n", f);
  constexpr size_t kNMetricas = std::size(kEsquemaMetricas);
  for (size_t i = 0; i < kNMetricas; ++i) {
    const Metrica& m = kEsquemaMetricas[i];
    (void)std::fputs("    ", f);
    escapa(f, m.chave);
    (void)std::fputs(": ", f);
    switch (m.forma) {
      case FormaValor::Escalar: {
        const Serie* s = serie_de(series, m.serie);
        if (publicavel(s)) {
          numero(f, s->mediana);
        } else {
          (void)std::fputs("null", f);
        }
        break;
      }
      case FormaValor::Objeto:
        objeto_de_nulos(f, m.subcampos);
        break;
      case FormaValor::Mapa:
        (void)std::fputs("{}", f);
        break;
    }
    (void)std::fputs(i + 1 < kNMetricas ? ",\n" : "\n", f);
  }
  (void)std::fputs("  },\n", f);
}

void escreve_ausentes(std::FILE* f) {
  (void)std::fputs("  \"metricas_ausentes\": {\n", f);
  bool primeiro = true;
  for (const Metrica& m : kEsquemaMetricas) {
    if (m.motivo == nullptr) continue;
    if (!primeiro) (void)std::fputs(",\n", f);
    primeiro = false;
    (void)std::fputs("    ", f);
    escapa(f, m.chave);
    (void)std::fputs(": ", f);
    escapa(f, m.motivo);
  }
  (void)std::fputs("\n  },\n", f);
}

}  // namespace

bool escreve_json(const std::string& caminho, const Ambiente& amb, const Config& cfg,
                  const DescricaoCarga& carga, const std::vector<Serie>& series,
                  const std::string& status, std::string& erro) {
  std::FILE* f = caminho.empty() ? stdout : std::fopen(caminho.c_str(), "w");
  if (f == nullptr) {
    erro = "não consegui abrir " + caminho + " para escrita";
    return false;
  }

  // O `schema` numera o formato dos DOIS documentos deste diretório (a medição e o baseline), e a
  // tabela do que mudou em cada versão está em bench/README.md. No 2 entrou o bloco `contrato`; no
  // 3, a lista `bloqueantes` dentro dele. Quem lê um documento de versão menor sabe que o bloco
  // não existe lá — e o relatório diz isso em vez de concluir que nada era obrigatório.
  (void)std::fputs("{\n  \"schema\": 3,\n  \"status\": ", f);
  escapa(f, status);
  (void)std::fprintf(f, ",\n  \"limiar_regressao_pct\": %.0f,\n", 5.0);

  (void)std::fputs("  \"ambiente\": {\n", f);
  (void)std::fputs("    \"commit\": ", f);
  escapa(f, amb.commit);
  (void)std::fputs(",\n    \"maquina\": ", f);
  escapa(f, amb.maquina);
  (void)std::fputs(",\n    \"kernel\": ", f);
  escapa(f, amb.kernel);
  (void)std::fputs(",\n    \"flags\": ", f);
  escapa(f, amb.flags);
  (void)std::fputs(",\n    \"dispositivo_wal\": ", f);
  escapa(f, amb.dispositivo_wal);
  (void)std::fprintf(f, ",\n    \"valido_para_baseline\": %s",
                     amb.valido_para_baseline() ? "true" : "false");
  if (!amb.valido_para_baseline()) {
    (void)std::fputs(",\n    \"por_que_invalido\": ", f);
    escapa(f, amb.por_que_invalido());
  }
  (void)std::fputs("\n  },\n", f);

  (void)std::fputs("  \"harness\": {\n", f);
  (void)std::fprintf(f, "    \"aquecimento\": %u,\n    \"repeticoes\": %u,\n", cfg.aquecimento,
                     cfg.repeticoes);
  (void)std::fprintf(f, "    \"limiar_cv_pct\": %.2f,\n    \"tentativas\": %u\n  },\n",
                     cfg.limiar_cv_pct, cfg.tentativas);

  // A carga vai no arquivo porque ela FAZ PARTE do número: o comparador recusa confrontar
  // medições de cargas diferentes, e sem estes campos ele não teria como saber.
  (void)std::fprintf(f,
                     "  \"carga\": { \"dias\": %u, \"negocios_por_dia\": %u, "
                     "\"investidores\": %u, \"particoes\": %u, \"semente\": %llu, "
                     "\"eventos\": %llu },\n",
                     carga.dias, carga.negocios_por_dia, carga.investidores, carga.particoes,
                     static_cast<unsigned long long>(carga.semente),
                     static_cast<unsigned long long>(carga.eventos));

  escreve_contrato(f, carga);
  escreve_metricas(f, series);
  escreve_ausentes(f);

  // ------------------------------------------------------------------ series
  (void)std::fputs("  \"series\": [\n", f);
  for (size_t i = 0; i < series.size(); ++i) {
    const Serie& s = series[i];
    (void)std::fputs("    { \"nome\": ", f);
    escapa(f, s.nome);
    (void)std::fputs(", \"grupo\": ", f);
    escapa(f, s.grupo);
    (void)std::fputs(", \"unidade\": ", f);
    escapa(f, s.unidade);
    (void)std::fprintf(f, ", \"direcao\": \"%s\"",
                       s.direcao == Direcao::MaiorMelhor ? "maior_melhor" : "menor_melhor");
    (void)std::fprintf(f, ", \"medida\": %s, \"estavel\": %s", s.medida ? "true" : "false",
                       s.estavel ? "true" : "false");
    if (s.medida) {
      (void)std::fputs(", \"mediana\": ", f);
      numero(f, s.mediana);
      (void)std::fputs(", \"media\": ", f);
      numero(f, s.media);
      (void)std::fputs(", \"minimo\": ", f);
      numero(f, s.minimo);
      (void)std::fputs(", \"maximo\": ", f);
      numero(f, s.maximo);
      (void)std::fputs(", \"cv_pct\": ", f);
      numero(f, s.cv_pct);
      (void)std::fprintf(f, ", \"series_descartadas\": %u", s.series_descartadas);
      (void)std::fprintf(f, ", \"operacoes_por_repeticao\": %llu",
                         static_cast<unsigned long long>(s.operacoes_por_repeticao));
      if (s.tem_quantis) {
        (void)std::fprintf(
            f,
            ", \"quantis_ns\": { \"p50\": %llu, \"p99\": %llu, \"p999\": %llu, "
            "\"max\": %llu }",
            static_cast<unsigned long long>(s.p50), static_cast<unsigned long long>(s.p99),
            static_cast<unsigned long long>(s.p999), static_cast<unsigned long long>(s.pmax));
      }
      (void)std::fputs(", \"amostras\": [", f);
      for (size_t k = 0; k < s.amostras.size(); ++k) {
        if (k != 0) (void)std::fputs(", ", f);
        numero(f, s.amostras[k]);
      }
      (void)std::fputs("]", f);
    }
    if (!s.nota.empty()) {
      (void)std::fputs(", \"nota\": ", f);
      escapa(f, s.nota);
    }
    (void)std::fputs(i + 1 < series.size() ? " },\n" : " }\n", f);
  }
  (void)std::fputs("  ]\n}\n", f);

  if (!caminho.empty()) (void)std::fclose(f);
  return true;
}

void imprime_tabela(const std::vector<Serie>& series) {
  (void)std::printf("\n%-8s %-46s %14s %-9s %7s %s\n", "grupo", "série", "mediana", "unidade",
                    "CV%", "");
  (void)std::printf("%s\n", std::string(102, '-').c_str());
  std::string grupo_atual;
  for (const Serie& s : series) {
    if (s.grupo != grupo_atual) {
      grupo_atual = s.grupo;
      (void)std::printf("\n");
    }
    if (!s.medida) {
      (void)std::printf("%-8s %-46s %14s %-9s %7s  %s\n", s.grupo.c_str(), s.nome.c_str(), "—",
                        s.unidade.c_str(), "—", s.nota.c_str());
      continue;
    }
    (void)std::printf("%-8s %-46s %14.2f %-9s %6.2f%%  %s\n", s.grupo.c_str(), s.nome.c_str(),
                      s.mediana, s.unidade.c_str(), s.cv_pct,
                      s.estavel ? "" : "INSTÁVEL — não vira baseline");
    if (s.tem_quantis) {
      (void)std::printf(
          "%-8s   %-44s p50=%llu  p99=%llu  p999=%llu  max=%llu (ns)\n", "", "quantis",
          static_cast<unsigned long long>(s.p50), static_cast<unsigned long long>(s.p99),
          static_cast<unsigned long long>(s.p999), static_cast<unsigned long long>(s.pmax));
    }
  }
  (void)std::printf("\n");
}

}  // namespace rv::bench
