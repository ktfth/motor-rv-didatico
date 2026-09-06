#include "bench/comparador.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rv::bench {
namespace {

// Um leitor recursivo do subconjunto que o esquema usa: objeto, texto, número, nulo, booleano,
// vetor. Ele achata o documento em caminhos ("a.b.c") porque é assim que o comparador endereça
// uma métrica — e porque uma árvore exigiria um DOM que nada mais aqui precisa.
class Leitor {
 public:
  explicit Leitor(std::string texto) : t_(std::move(texto)) {}

  [[nodiscard]] bool documento(DocumentoJson& out, std::string& erro) {
    espaco();
    if (!objeto("", out, erro)) return false;
    return true;
  }

 private:
  void espaco() {
    while (i_ < t_.size() && (std::isspace(static_cast<unsigned char>(t_[i_])) != 0)) ++i_;
  }
  [[nodiscard]] bool consome(char c) {
    espaco();
    if (i_ < t_.size() && t_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  [[nodiscard]] bool texto_json(std::string& out, std::string& erro) {
    espaco();
    if (i_ >= t_.size() || t_[i_] != '"') {
      erro = "esperava texto na posição " + std::to_string(i_);
      return false;
    }
    ++i_;
    out.clear();
    while (i_ < t_.size() && t_[i_] != '"') {
      if (t_[i_] == '\\' && i_ + 1 < t_.size()) {
        ++i_;
        switch (t_[i_]) {
          case 'n':
            out += '\n';
            break;
          case 't':
            out += '\t';
            break;
          case 'r':
            out += '\r';
            break;
          case 'u':
            i_ += 4;
            out += '?';
            break;  // o esquema não usa; não invento tradução
          default:
            out += t_[i_];
        }
      } else {
        out += t_[i_];
      }
      ++i_;
    }
    if (i_ >= t_.size()) {
      erro = "texto sem fechamento";
      return false;
    }
    ++i_;
    return true;
  }

  [[nodiscard]] bool valor(const std::string& caminho, DocumentoJson& out, std::string& erro) {
    espaco();
    if (i_ >= t_.size()) {
      erro = "documento truncado";
      return false;
    }
    const char c = t_[i_];
    if (c == '{') return objeto(caminho, out, erro);
    if (c == '[') return vetor(caminho, out, erro);
    if (c == '"') {
      std::string s;
      if (!texto_json(s, erro)) return false;
      ValorJson v;
      v.tipo = ValorJson::Tipo::Texto;
      v.texto = s;
      out[caminho] = v;
      return true;
    }
    if (t_.compare(i_, 4, "null") == 0) {
      i_ += 4;
      out[caminho] = ValorJson{};
      return true;
    }
    if (t_.compare(i_, 4, "true") == 0 || t_.compare(i_, 5, "false") == 0) {
      const bool b = t_[i_] == 't';
      i_ += b ? 4 : 5;
      ValorJson v;
      v.tipo = ValorJson::Tipo::Texto;
      v.texto = b ? "true" : "false";
      out[caminho] = v;
      return true;
    }
    // número
    const size_t inicio = i_;
    while (i_ < t_.size() &&
           (std::isdigit(static_cast<unsigned char>(t_[i_])) != 0 || t_[i_] == '-' ||
            t_[i_] == '+' || t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E')) {
      ++i_;
    }
    if (i_ == inicio) {
      erro = "valor irreconhecível na posição " + std::to_string(i_);
      return false;
    }
    ValorJson v;
    v.tipo = ValorJson::Tipo::Numero;
    v.numero = std::strtod(t_.substr(inicio, i_ - inicio).c_str(), nullptr);
    out[caminho] = v;
    return true;
  }

  [[nodiscard]] bool objeto(const std::string& caminho, DocumentoJson& out, std::string& erro) {
    if (!consome('{')) {
      erro = "esperava '{' na posição " + std::to_string(i_);
      return false;
    }
    espaco();
    if (consome('}')) return true;
    while (true) {
      std::string chave;
      if (!texto_json(chave, erro)) return false;
      if (!consome(':')) {
        erro = "esperava ':' depois de " + chave;
        return false;
      }
      std::string filho = caminho;
      if (!filho.empty()) filho += '.';
      filho += chave;
      if (!valor(filho, out, erro)) return false;
      espaco();
      if (consome(',')) continue;
      if (consome('}')) return true;
      erro = "esperava ',' ou '}' na posição " + std::to_string(i_);
      return false;
    }
  }

  [[nodiscard]] bool vetor(const std::string& caminho, DocumentoJson& out, std::string& erro) {
    if (!consome('[')) return false;
    espaco();
    if (consome(']')) return true;
    uint32_t k = 0;
    std::string filho;
    while (true) {
      filho = caminho;
      filho += '[';
      filho += std::to_string(k);
      filho += ']';
      if (!valor(filho, out, erro)) return false;
      ++k;
      espaco();
      if (consome(',')) continue;
      if (consome(']')) return true;
      erro = "esperava ',' ou ']' na posição " + std::to_string(i_);
      return false;
    }
  }

  std::string t_;
  size_t i_ = 0;
};

// A ponte série -> chave do baseline. É a MESMA de saida.cpp e por um bom motivo: se o comparador
// usasse outra, ele compararia uma métrica com o baseline de outra, e o gate passaria verde
// comparando coisa nenhuma.
struct Ponte {
  const char* serie;
  const char* chave;
};
constexpr Ponte kPontes[] = {
    {"nucleo.loop.eventos_por_s_por_core", "metricas.nucleo.eventos_por_s_por_core"},
    {"snapshot.salva.duracao_ms", "metricas.snapshot.duracao_ms"},
};

}  // namespace

bool le_json(const std::string& caminho, DocumentoJson& out, std::string& erro) {
  std::ifstream f(caminho);
  if (!f) {
    erro = "não consegui abrir " + caminho;
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  Leitor l{ss.str()};
  return l.documento(out, erro);
}

namespace {

// Confere um campo da carga contra o baseline. Campo AUSENTE no baseline é aceito com aviso: um
// baseline antigo, gravado antes de a carga ser registrada, não deve derrubar o gate — mas
// também não pode passar como se tivesse sido conferido.
void confere_campo(const DocumentoJson& baseline, const char* chave, uint64_t atual, Veredito& v) {
  const auto it = baseline.find(chave);
  if (it == baseline.end() || it->second.tipo != ValorJson::Tipo::Numero) return;
  const auto do_baseline = static_cast<uint64_t>(it->second.numero);
  if (do_baseline == atual) return;
  v.carga_incompativel = true;
  if (!v.motivo_carga.empty()) v.motivo_carga += ", ";
  v.motivo_carga += chave;
  v.motivo_carga += " (baseline ";
  v.motivo_carga += std::to_string(do_baseline);
  v.motivo_carga += ", agora ";
  v.motivo_carga += std::to_string(atual);
  v.motivo_carga += ")";
}

}  // namespace

Veredito compara(const DocumentoJson& baseline, const std::vector<Serie>& series, double limiar_pct,
                 const DescricaoCarga& carga) {
  Veredito v;

  if (!carga.vazia()) {
    confere_campo(baseline, "carga.dias", carga.dias, v);
    confere_campo(baseline, "carga.negocios_por_dia", carga.negocios_por_dia, v);
    confere_campo(baseline, "carga.investidores", carga.investidores, v);
    confere_campo(baseline, "carga.particoes", carga.particoes, v);
    confere_campo(baseline, "carga.semente", carga.semente, v);
    if (v.carga_incompativel) return v;
  }
  for (const Ponte& p : kPontes) {
    const Serie* s = nullptr;
    for (const Serie& cand : series) {
      if (cand.nome == p.serie) {
        s = &cand;
        break;
      }
    }
    if (s == nullptr || !s->medida || !s->estavel) continue;

    const auto it = baseline.find(p.chave);
    if (it == baseline.end() || it->second.tipo != ValorJson::Tipo::Numero) {
      v.sem_baseline.push_back(p.chave);
      continue;
    }
    v.baseline_vazio = false;

    Comparacao c;
    c.chave = p.chave;
    c.baseline = it->second.numero;
    c.medido = s->mediana;
    c.direcao = s->direcao;
    if (c.baseline != 0.0) {
      c.variacao_pct = 100.0 * (c.medido - c.baseline) / c.baseline;
    }
    // A direção é o que faz o sinal significar alguma coisa: −8 % em eventos/s é regressão; −8 %
    // em milissegundos de snapshot é ganho.
    c.regressao = (c.direcao == Direcao::MaiorMelhor) ? (c.variacao_pct < -limiar_pct)
                                                      : (c.variacao_pct > limiar_pct);
    if (c.regressao) v.houve_regressao = true;
    v.comparacoes.push_back(c);
  }
  return v;
}

void imprime_veredito(const Veredito& v, double limiar_pct) {
  (void)std::printf("\n== comparação com bench/baseline.json (limiar %.0f%%) ==\n", limiar_pct);
  if (v.carga_incompativel) {
    (void)std::printf(
        "  CARGA DIFERENTE da do baseline: %s\n"
        "  Nada comparado. Um pregão e três pregões dão números diferentes da MESMA\n"
        "  métrica; confrontá-los mediria a diferença entre dois experimentos.\n",
        v.motivo_carga.c_str());
    return;
  }
  if (v.comparacoes.empty() && v.sem_baseline.empty()) {
    (void)std::printf("  nenhuma métrica desta execução tem chave no baseline.\n");
    return;
  }
  for (const std::string& k : v.sem_baseline) {
    (void)std::printf("  %-44s SEM BASELINE — nada a comparar\n", k.c_str());
  }
  for (const Comparacao& c : v.comparacoes) {
    (void)std::printf("  %-44s baseline %12.2f  medido %12.2f  %+7.2f%%  %s\n", c.chave.c_str(),
                      c.baseline, c.medido, c.variacao_pct, c.regressao ? "REGRESSÃO" : "ok");
  }
  if (v.houve_regressao) {
    (void)std::printf(
        "\n  Há regressão acima do limiar. ADR-0016: nenhuma otimização é aprovável\n"
        "  contra um baseline que ela mesma derrubou.\n");
  }
}

}  // namespace rv::bench
