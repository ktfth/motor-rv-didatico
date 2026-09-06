// A saída do harness: o MESMO esquema de bench/baseline.json, mais o detalhe das séries.
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
// que peça de código ainda não existe. Quando a peça existir, a linha some sozinha — porque a
// série passa a ser medida e o mapeamento a preenche.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bench/harness.hpp"

namespace rv::bench {
namespace {

// A ponte entre uma série medida e a chave que `bench/baseline.json` publica. Explícita, e não
// por convenção de nome: a chave do baseline é contrato (o comparador a lê), o nome da série é
// organização interna. Renomear uma série não pode mudar o contrato em silêncio.
struct Ponte {
  const char* serie;
  const char* chave_baseline;
};

constexpr Ponte kPontes[] = {
    {"nucleo.loop.eventos_por_s_por_core", "nucleo.eventos_por_s_por_core"},
    {"snapshot.salva.duracao_ms", "snapshot.duracao_ms"},
};

// As métricas do esquema que este binário ainda NÃO consegue medir, e por quê. A frase é o
// próximo passo do projeto, não uma desculpa.
struct Ausente {
  const char* chave;
  const char* motivo;
};

constexpr Ausente kAusentes[] = {
    {"wal.append_para_duravel_us",
     "o escritor do WAL (segment/group_commit/wal) não existe ainda; só o formato e os backends. "
     "O piso físico do dispositivo está nas séries wal.*_backend_*"},
    {"wal.tamanho_grupo_bytes", "não há group commit para agrupar: mesma pendência"},
    {"wal.grupos_em_voo", "não há group commit: mesma pendência"},
    {"wal.recuperacao_s",
     "não há wal/recovery.cpp; a velocidade de reaplicação está em nucleo.apply.eventos_por_s, "
     "que é o limite superior (sem custo de leitura de disco)"},
    {"rs.latencia_por_endpoint_ms", "src/edge/ não existe (fase 4)"},
};

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

[[nodiscard]] const Serie* acha(const std::vector<Serie>& series, const char* nome) {
  for (const Serie& s : series) {
    if (s.nome == nome) return &s;
  }
  return nullptr;
}

// Uma série só vira número de baseline se foi medida E ficou estável. É a regra de ADR-0021
// aplicada no ponto onde ela vale: na escrita do arquivo, não na cabeça de quem lê o terminal.
[[nodiscard]] bool publicavel(const Serie* s) noexcept {
  return s != nullptr && s->medida && s->estavel;
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

  (void)std::fputs("{\n  \"schema\": 1,\n  \"status\": ", f);
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

  // ------------------------------------------------------------------ metricas
  // A ordem e as chaves são as de bench/baseline.json, inclusive as nulas: o comparador casa por
  // chave, e um arquivo de medição com esquema diferente do baseline não compara nada.
  const auto por_chave = [&series](const char* chave) -> const Serie* {
    for (const Ponte& p : kPontes) {
      if (std::string(p.chave_baseline) == chave) return acha(series, p.serie);
    }
    return nullptr;
  };
  const Serie* nucleo = por_chave("nucleo.eventos_por_s_por_core");
  const Serie* snap = por_chave("snapshot.duracao_ms");

  (void)std::fputs("  \"metricas\": {\n    \"nucleo.eventos_por_s_por_core\": ", f);
  if (publicavel(nucleo))
    numero(f, nucleo->mediana);
  else
    (void)std::fputs("null", f);
  (void)std::fputs(
      ",\n    \"wal.append_para_duravel_us\": "
      "{ \"p50\": null, \"p99\": null, \"p999\": null },\n",
      f);
  (void)std::fputs("    \"wal.tamanho_grupo_bytes\": { \"p50\": null, \"p99\": null },\n", f);
  (void)std::fputs("    \"wal.grupos_em_voo\": { \"p50\": null, \"max\": null },\n", f);
  (void)std::fputs("    \"wal.recuperacao_s\": null,\n    \"snapshot.duracao_ms\": ", f);
  if (publicavel(snap))
    numero(f, snap->mediana);
  else
    (void)std::fputs("null", f);
  (void)std::fputs(",\n    \"rs.latencia_por_endpoint_ms\": {}\n  },\n", f);

  (void)std::fputs("  \"metricas_ausentes\": {\n", f);
  constexpr size_t kNAusentes = sizeof(kAusentes) / sizeof(kAusentes[0]);
  for (size_t i = 0; i < kNAusentes; ++i) {
    (void)std::fputs("    ", f);
    escapa(f, kAusentes[i].chave);
    (void)std::fputs(": ", f);
    escapa(f, kAusentes[i].motivo);
    (void)std::fputs(i + 1 < kNAusentes ? ",\n" : "\n", f);
  }
  (void)std::fputs("  },\n", f);

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
